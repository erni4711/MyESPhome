#include "ha_ws_client.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <set>

#include "../hatilvgl/tiles_lvgl.h"

static const char* TAG = "ha_ws_client";  // "hatilvgl";

namespace web_admin_local {

namespace {

// Non-state protocol messages are expected to be small. get_states replies
// are parsed incrementally and never stored as one complete WebSocket message.
constexpr size_t kMaxMessageBytes = 32684;
constexpr size_t kStreamProbeBytes = 512;
constexpr size_t kMaxStateObjectBytes = 65536;
constexpr size_t kMaxQueuedJsonBytes = 65536;  // queue item storage is PSRAM-backed
constexpr size_t kQueueLength = 4;
constexpr int kGetStatesId = 2;

struct QueuedJson {
  size_t length;
  char data[kMaxQueuedJsonBytes];
};

template <typename T>
struct PsramAllocator {
  using value_type = T;

  T* allocate(std::size_t count) {
    void* memory = heap_caps_malloc(count * sizeof(T),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) std::abort();
    return static_cast<T*>(memory);
  }

  void deallocate(T* pointer, std::size_t) { heap_caps_free(pointer); }

  template <typename U>
  bool operator==(const PsramAllocator<U>&) const {
    return true;
  }

  template <typename U>
  bool operator!=(const PsramAllocator<U>&) const {
    return false;
  }
};

using PsramString =
    std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

class PsramJsonAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  void deallocate(void* pointer) override { heap_caps_free(pointer); }

  void* reallocate(void* pointer, size_t new_size) override {
    return heap_caps_realloc(pointer, new_size,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};

PsramJsonAllocator g_psram_json_allocator;

// These buffers are shared only by their respective single producer/consumer
// tasks. Keeping them static avoids placing an 8 KiB queue item on a task
// stack.
QueuedJson* g_enqueue_item = nullptr;
QueuedJson* g_consume_item = nullptr;
uint8_t* g_queue_storage = nullptr;

esp_websocket_client_handle_t g_client = nullptr;
std::string g_url;
std::string g_token;
bool g_started = false;
bool g_authenticated = false;
bool g_subscribed = false;
PsramString g_rx_buffer;
std::string g_stream_probe;

std::string g_state_object;
bool g_get_states_stream = false;
bool g_get_states_stream_complete = false;
int g_state_depth = 0;
bool g_state_in_string = false;
bool g_state_escaped = false;
bool g_state_oversized = false;
uint32_t g_stream_state_count = 0;

bool allocate_queue_items() {
  if (g_enqueue_item != nullptr && g_consume_item != nullptr) return true;

  g_enqueue_item = static_cast<QueuedJson*>(heap_caps_calloc(
      1, sizeof(QueuedJson), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_consume_item = static_cast<QueuedJson*>(heap_caps_calloc(
      1, sizeof(QueuedJson), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_enqueue_item == nullptr || g_consume_item == nullptr) {
    heap_caps_free(g_enqueue_item);
    heap_caps_free(g_consume_item);
    g_enqueue_item = nullptr;
    g_consume_item = nullptr;
    ESP_LOGE(TAG, "Unable to allocate Home Assistant queue buffers in PSRAM");
    return false;
  }
  return true;
}

void log_websocket_message(const char* direction, const char* message,
                           size_t size) {
  // Keep protocol logging useful without flooding the ESPHome log with large
  // get_states responses. Authentication tokens must never be logged.

  // Keep the complete formatted log record below ESPHome's default printf
  // buffer size; payloads themselves may be much larger.
  constexpr size_t kLogPreviewBytes = 128;
  const size_t preview_size = size < kLogPreviewBytes ? size : kLogPreviewBytes;
  ESP_LOGD(TAG, "WebSocket %s: %u bytes%s: %.*s", direction,
           static_cast<unsigned>(size), size > preview_size ? " (preview)" : "",
           static_cast<int>(preview_size), message);
}

QueueHandle_t update_queue() {
  static StaticQueue_t queue_control;
  if (g_queue_storage == nullptr) {
    g_queue_storage = static_cast<uint8_t*>(heap_caps_calloc(
        kQueueLength, sizeof(QueuedJson), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_queue_storage == nullptr) {
      ESP_LOGE(TAG, "Unable to allocate Home Assistant queue storage in PSRAM");
      return nullptr;
    }
  }
  static QueueHandle_t queue = xQueueCreateStatic(
      kQueueLength, sizeof(QueuedJson), g_queue_storage, &queue_control);
  return queue;
}

SemaphoreHandle_t filter_mutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

std::set<std::string>& entity_filter() {
  static std::set<std::string> filter;
  return filter;
}

bool entity_is_interesting(const char* entity_id) {
  SemaphoreHandle_t mtx = filter_mutex();
  xSemaphoreTake(mtx, portMAX_DELAY);
  const bool found = entity_filter().count(entity_id) > 0;
  xSemaphoreGive(mtx);
  return found;
}

void enqueue_json(JsonObject state) {
  const char* entity_id = state["entity_id"] | "";
  if (g_enqueue_item == nullptr) {
    ESP_LOGE(TAG, "Unable to queue Home Assistant state: buffer unavailable");
    return;
  }
  *g_enqueue_item = {};
  g_enqueue_item->length =
      serializeJson(state, g_enqueue_item->data, sizeof(g_enqueue_item->data));
  if (g_enqueue_item->length == 0 ||
      g_enqueue_item->length >= sizeof(g_enqueue_item->data)) {
    ESP_LOGW(TAG, "State JSON for %s exceeds %u bytes; dropping update",
             entity_id,
             static_cast<unsigned>(sizeof(g_enqueue_item->data) - 1));
    return;
  }
  QueueHandle_t queue = update_queue();
  if (queue == nullptr) {
    ESP_LOGE(TAG, "Unable to queue Home Assistant state: queue unavailable");
    return;
  }
  if (xQueueSend(queue, g_enqueue_item, 0) != pdTRUE) {
    ESP_LOGW(TAG, "State JSON queue full, dropping update for %s", entity_id);
    return;
  }
  ESP_LOGD(TAG, "Queued Home Assistant state for %s (%u bytes)", entity_id,
           static_cast<unsigned>(g_enqueue_item->length));
}

// Copies one interesting state into the fixed-size queue. The JSON document
// itself remains owned by the websocket task until this function returns.
void handle_new_state(JsonObject new_state, const char* source) {
  const char* entity_id = new_state["entity_id"] | "";
  const bool interesting =
      entity_id[0] != '\0' && entity_is_interesting(entity_id);
  if (!interesting) return;
  ESP_LOGD(TAG, "Accepted Home Assistant state: entity_id=%s source=%s",
           entity_id, source);
  enqueue_json(new_state);
}

void handle_json_document(JsonDocument& doc) {
  const char* type = doc["type"] | "";
  ESP_LOGD(TAG, "Received Home Assistant JSON type=%s bare_state=%d",
           type[0] ? type : "(none)",
           doc["entity_id"].is<const char*>() ? 1 : 0);
  // A bare state object (no "type" field) comes from a get_states reply
  // that was streamed as one JSON document per state instead of a single
  // result array. Treat it like any other state update.
  if (type[0] == '\0' && doc["entity_id"].is<const char*>()) {
    handle_new_state(doc.as<JsonObject>(), "stream");
    return;
  }
  if (std::strcmp(type, "auth_required") == 0) {
    // Reply with the configured long-lived access token. Never log it.
    std::string auth =
        std::string("{\"type\":\"auth\",\"access_token\":\"") + g_token + "\"}";
    ESP_LOGD(TAG, "WebSocket TX: auth request (access token omitted)");
    esp_websocket_client_send_text(g_client, auth.c_str(),
                                   static_cast<int>(auth.size()),
                                   pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Sent Home Assistant websocket auth request");
  } else if (std::strcmp(type, "auth_ok") == 0) {
    if (!g_authenticated) {
      g_authenticated = true;
      ESP_LOGI(TAG, "Home Assistant websocket authenticated");
    }
  } else if (std::strcmp(type, "auth_invalid") == 0) {
    g_authenticated = false;
    ESP_LOGW(TAG,
             "Home Assistant websocket authentication rejected; check the "
             "configured token");
  } else if (std::strcmp(type, "event") == 0) {
    JsonVariant event = doc["event"];
    if (event.is<JsonObject>()) {
      JsonObject event_obj = event.as<JsonObject>();
      const char* event_type = event_obj["event_type"] | "";
      if (std::strcmp(event_type, "state_changed") == 0) {
        JsonVariant new_state = event_obj["data"]["new_state"];
        if (new_state.is<JsonObject>()) {
          handle_new_state(new_state.as<JsonObject>(), "event");
        }
      }
    }
  } else if (std::strcmp(type, "result") == 0) {
    const int id = doc["id"] | 0;
    const bool success = doc["success"] | false;
    if (id == kGetStatesId && success) {
      JsonVariant result = doc["result"];
      if (result.is<JsonArray>()) {
        for (JsonObject state : result.as<JsonArray>()) {
          handle_new_state(state, "initial");
        }
      } else if (result.is<JsonObject>() &&
                 result["entity_id"].is<const char*>()) {
        // Single-state result: streamed one JSON object per state.
        handle_new_state(result.as<JsonObject>(), "initial");
      }
    }
  }
}

void handle_text_message(const char* message, size_t message_size) {
  log_websocket_message("RX", message, message_size);
  JsonDocument doc(&g_psram_json_allocator);
  const DeserializationError err = deserializeJson(doc, message, message_size);
  if (err) {
    ESP_LOGW(TAG,
             "Failed to parse Home Assistant websocket message (%s), %u bytes",
             err.c_str(), static_cast<unsigned>(message_size));
    return;
  }
  handle_json_document(doc);
}

void reset_connection_state() {
  g_authenticated = false;
  g_rx_buffer.clear();
  g_stream_probe.clear();
  g_state_object.clear();
  g_get_states_stream = false;
  g_get_states_stream_complete = false;
  g_state_depth = 0;
  g_state_in_string = false;
  g_state_escaped = false;
  g_state_oversized = false;
  g_stream_state_count = 0;
}

size_t find_result_array_start(const std::string& probe) {
  const size_t result_key = probe.find("\"result\"");
  if (result_key == std::string::npos) return std::string::npos;
  const size_t colon = probe.find(':', result_key + 8);
  if (colon == std::string::npos) return std::string::npos;
  const size_t array = probe.find('[', colon + 1);
  return array == std::string::npos ? std::string::npos : array + 1;
}

void parse_streamed_state(const std::string& state_json) {
  JsonDocument state_doc(&g_psram_json_allocator);
  const DeserializationError err = deserializeJson(state_doc, state_json);
  if (err) {
    //std::printf("[ha_ws] streamed state parse failed bytes=%u error=%s\n",
    //            static_cast<unsigned>(state_json.size()), err.c_str());
    ESP_LOGW(TAG,
             "Failed to parse streamed Home Assistant state (%s), %u bytes",
             err.c_str(), static_cast<unsigned>(state_json.size()));
    return;
  }
  if (state_doc.is<JsonObject>() && state_doc["entity_id"].is<const char*>()) {
    //std::printf("[ha_ws] streamed state complete entity=%s bytes=%u\n",
    //            state_doc["entity_id"].as<const char*>(),
    //            static_cast<unsigned>(state_json.size()));
    handle_new_state(state_doc.as<JsonObject>(), "initial");
  } else {
    //std::printf("[ha_ws] streamed state missing entity_id bytes=%u\n",
    //            static_cast<unsigned>(state_json.size()));
  }
}

void consume_get_states_stream(const char* data, size_t size) {
  //std::printf("[ha_ws] consume states chunk bytes=%u depth=%d count=%u\n",
  //            static_cast<unsigned>(size), g_state_depth,
  //            static_cast<unsigned>(g_stream_state_count));
  for (size_t i = 0; i < size; ++i) {
    const char ch = data[i];
    if (g_get_states_stream_complete) continue;

    if (!g_state_depth) {
      if (ch == '{') {
        g_state_object.clear();
        g_state_object.push_back(ch);
        g_state_depth = 1;
        g_state_in_string = false;
        g_state_escaped = false;
        g_state_oversized = false;
        //std::printf("[ha_ws] streamed state start\n");
      } else if (ch == ']') {
        g_get_states_stream_complete = true;
        //std::printf("[ha_ws] streamed states array complete\n");
      }
      continue;
    }

    if (!g_state_oversized &&
        g_state_object.size() >= kMaxStateObjectBytes) {
      ESP_LOGW(TAG,
               "Streamed Home Assistant state exceeds %u bytes; dropping it",
               static_cast<unsigned>(kMaxStateObjectBytes));
      //std::printf("[ha_ws] streamed state oversized depth=%d; skipping\n",
      //    g_state_depth);
      g_state_oversized = true;
    }
    if (!g_state_oversized) g_state_object.push_back(ch);

    if (g_state_in_string) {
      if (g_state_escaped) {
        g_state_escaped = false;
      } else if (ch == '\\') {
        g_state_escaped = true;
      } else if (ch == '"') {
        g_state_in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      g_state_in_string = true;
    } else if (ch == '{' || ch == '[') {
      ++g_state_depth;
    } else if (ch == '}' || ch == ']') {
      --g_state_depth;
      if (g_state_depth == 0) {
        ++g_stream_state_count;
        //std::printf(
        //    "[ha_ws] consume state #%u complete bytes=%u oversized=%d\n",
        //    static_cast<unsigned>(g_stream_state_count),
        //    static_cast<unsigned>(g_state_object.size()),
        //    g_state_oversized ? 1 : 0);
        if (!g_state_oversized) {
          parse_streamed_state(g_state_object);
        } else {
          //    std::printf("[ha_ws] streamed oversized state complete; resumed\n");
        }
        g_state_object.clear();
        g_state_oversized = false;
      }
    }
  }
}

void handle_websocket_data(const char* message, size_t message_size,
                           size_t payload_size, bool final_chunk) {
  if (!g_get_states_stream) {
    const size_t probe_size_before = g_stream_probe.size();
    const size_t probe_bytes =
        std::min(message_size, kStreamProbeBytes - probe_size_before);
    if (g_stream_probe.size() < kStreamProbeBytes) {
      g_stream_probe.append(message, probe_bytes);
    }
    if (payload_size <= kMaxMessageBytes) {
      g_rx_buffer.append(message, message_size);
    }
    const size_t stream_start = find_result_array_start(g_stream_probe);
    if (stream_start != std::string::npos &&
        g_stream_probe.find("\"id\":2") != std::string::npos &&
        g_stream_probe.find("\"type\":\"result\"") != std::string::npos) {
      g_get_states_stream = true;
      const std::string tail = g_stream_probe.substr(stream_start);
      g_rx_buffer.clear();
      consume_get_states_stream(tail.data(), tail.size());
      // The probe may end in the middle of this WebSocket fragment. Continue
      // with the unprobed suffix instead of dropping the remaining states.
      if (probe_bytes < message_size) {
        consume_get_states_stream(message + probe_bytes,
                                  message_size - probe_bytes);
      }
    } else if (final_chunk) {
      if (payload_size <= kMaxMessageBytes) {
        handle_text_message(g_rx_buffer.data(), g_rx_buffer.size());
      } else {
        ESP_LOGW(
            TAG,
            "Unable to identify oversized Home Assistant websocket message");
      }
    }
  } else {
    consume_get_states_stream(message, message_size);
  }

  if (final_chunk) {
    g_rx_buffer.clear();
    g_stream_probe.clear();
    g_state_object.clear();
    g_get_states_stream = false;
    g_get_states_stream_complete = false;
    g_state_depth = 0;
    g_state_in_string = false;
    g_state_escaped = false;
    g_state_oversized = false;
    g_stream_state_count = 0;
  }
}

void ha_ws_event_handler(void* handler_args, esp_event_base_t base,
                         int32_t event_id, void* event_data) {
  (void)handler_args;
  (void)base;

  auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
  switch (static_cast<esp_websocket_event_id_t>(event_id)) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "Home Assistant websocket connected");
      reset_connection_state();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGW(TAG, "Home Assistant websocket disconnected");
      reset_connection_state();
      break;
    case WEBSOCKET_EVENT_DATA: {
      if (data == nullptr || data->data_ptr == nullptr) break;
      // Only text frames carry the JSON protocol payload.
      if (data->op_code != 0x01) break;
      if (data->payload_len <= 0) break;

      ESP_LOGD(TAG, "WebSocket RX frame: %u/%u bytes at offset %u",
               static_cast<unsigned>(data->data_len),
               static_cast<unsigned>(data->payload_len),
               static_cast<unsigned>(data->payload_offset));
      //std::printf("[ha_ws] data opcode=%u bytes=%u payload=%u offset=%u\n",
      //            static_cast<unsigned>(data->op_code),
      //            static_cast<unsigned>(data->data_len),
      //            static_cast<unsigned>(data->payload_len),
      //            static_cast<unsigned>(data->payload_offset));

      if (data->payload_offset == 0) {
        g_rx_buffer.clear();
        g_stream_probe.clear();
        g_state_object.clear();
        g_get_states_stream = false;
        g_get_states_stream_complete = false;
        g_state_depth = 0;
        g_state_in_string = false;
        g_state_escaped = false;
        g_state_oversized = false;
      }
      const bool final_chunk =
          data->payload_offset + data->data_len >= data->payload_len;
      if (!g_get_states_stream &&
          g_rx_buffer.size() + static_cast<size_t>(data->data_len) >
              kMaxMessageBytes &&
          data->payload_len <= kMaxMessageBytes) {
        ESP_LOGW(
            TAG,
            "Home Assistant websocket message exceeds %u bytes; dropping it",
            static_cast<unsigned>(kMaxMessageBytes));
        if (final_chunk) g_rx_buffer.clear();
        break;
      }
      handle_websocket_data(data->data_ptr, static_cast<size_t>(data->data_len),
                            static_cast<size_t>(data->payload_len),
                            final_chunk);
      break;
    }
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "Home Assistant websocket error (type=%d)",
               data ? static_cast<int>(data->error_handle.error_type) : -1);
      break;
    default:
      break;
  }
}

// Builds the `ws://` or `wss://` URI for `/api/websocket` from a Home
// Assistant base URL such as "http://homeassistant.local:8123". Any path
// the user included is dropped; the websocket API always lives at
// `/api/websocket` on the configured host:port.
std::string build_uri(const std::string& home_assistant_url, bool* out_is_tls) {
  std::string url = home_assistant_url;
  bool is_tls = false;
  const std::string https_prefix = "https://";
  const std::string http_prefix = "http://";
  if (url.rfind(https_prefix, 0) == 0) {
    is_tls = true;
    url = url.substr(https_prefix.size());
  } else if (url.rfind(http_prefix, 0) == 0) {
    url = url.substr(http_prefix.size());
  }
  const size_t slash = url.find('/');
  if (slash != std::string::npos) url = url.substr(0, slash);
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (out_is_tls) *out_is_tls = is_tls;
  return (is_tls ? std::string("wss://") : std::string("ws://")) + url +
         "/api/websocket";
}

}  // namespace

void ha_ws_client_configure(const std::string& home_assistant_url,
                            const std::string& home_assistant_token) {
  ESP_LOGI(TAG, "ha_ws_client_configure entered (url=%s, token_present=%s)",
           home_assistant_url.empty() ? "empty" : "set",
           home_assistant_token.empty() ? "no" : "yes");
  ESP_LOGI(TAG, "Configuring Home Assistant websocket client: %s",
           home_assistant_url.c_str());
  g_url = home_assistant_url;
  g_token = home_assistant_token;
}

void ha_ws_client_set_entity_filter(
    const std::vector<std::string>& entity_ids) {
  SemaphoreHandle_t mtx = filter_mutex();
  xSemaphoreTake(mtx, portMAX_DELAY);
  const std::set<std::string> updated_filter(entity_ids.begin(),
                                             entity_ids.end());
  entity_filter() = updated_filter;
  xSemaphoreGive(mtx);
  ESP_LOGI(TAG, "Home Assistant websocket filter now tracks %u entit%s",
           static_cast<unsigned>(entity_ids.size()),
           entity_ids.size() == 1 ? "y" : "ies");
  for (const auto& entity_id : entity_ids) {
    ESP_LOGI(TAG, "Home Assistant websocket subscribed entity: %s",
             entity_id.c_str());
  }
}

void ha_ws_client_unsubscribe_events() {
  if (!g_authenticated || g_client == nullptr) return;
  if (!g_subscribed) {
    ESP_LOGI(TAG, "Not subscribed to Home Assistant state_changed events");
    return;
  }
  static const char* kUnsubscribe =
      "{\"id\":1,\"type\":\"unsubscribe_events\",\"event_type\":\"state_"
      "changed\"}";
  ESP_LOGD(TAG,
           "WebSocket TX: unsubscribe_events (id=1, event_type=state_changed)");
  esp_websocket_client_send_text(g_client, kUnsubscribe,
                                 static_cast<int>(std::strlen(kUnsubscribe)),
                                 pdMS_TO_TICKS(5000));
  ESP_LOGI(TAG, "Unsubscribed from Home Assistant state_changed events (id=1)");
  g_subscribed = false;
}
void ha_ws_client_subscribe_events() {
  ESP_LOGD(TAG, "Subscribe requested (authenticated=%d subscribed=%d)",
           g_authenticated ? 1 : 0, g_subscribed ? 1 : 0);
  if (!g_authenticated || g_client == nullptr) return;
  if (g_subscribed) {
    ESP_LOGI(TAG, "Already subscribed to Home Assistant state_changed events");
    return;
  }
  static const char* kSubscribe =
      "{\"id\":1,\"type\":\"subscribe_events\",\"event_type\":\"state_"
      "changed\"}";
  ESP_LOGD(TAG,
           "WebSocket TX: subscribe_events (id=1, event_type=state_changed)");
  esp_websocket_client_send_text(g_client, kSubscribe,
                                 static_cast<int>(std::strlen(kSubscribe)),
                                 pdMS_TO_TICKS(5000));
  g_subscribed = true;
  ESP_LOGI(TAG, "Subscribed to Home Assistant state_changed events (id=1)");
}

void ha_ws_client_request_states() {
  ESP_LOGI(TAG,
           "Requesting current Home Assistant entity states (authenticated=%d, "
           "client=%p)",
           g_authenticated, g_client);
  if (!g_authenticated || g_client == nullptr) return;
  static const char* kGetStates = "{\"id\":2,\"type\":\"get_states\"}";
  const int sent = esp_websocket_client_send_text(
      g_client, kGetStates, static_cast<int>(std::strlen(kGetStates)),
      pdMS_TO_TICKS(5000));
  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to request current Home Assistant entity states");
    return;
  }
  ESP_LOGD(TAG, "WebSocket TX: %s", kGetStates);
  ESP_LOGI(TAG, "Requested current Home Assistant entity states (id=2)");
}

void ha_ws_client_discard_pending_states() {
  if (!g_started || g_consume_item == nullptr) return;
  QueueHandle_t queue = update_queue();
  if (queue == nullptr) {
    ESP_LOGE(TAG, "Unable to discard Home Assistant states: queue unavailable");
    return;
  }
  int discarded = 0;
  while (xQueueReceive(queue, g_consume_item, 0) == pdTRUE) {
    ++discarded;
  }
  if (discarded > 0) {
    ESP_LOGD(TAG, "Discarded %d stale Home Assistant state updates", discarded);
  }
}

void ha_ws_client_start() {
  ESP_LOGI(TAG, "ha_ws_client_start entered (configured=%s, started=%s)",
           g_url.empty() || g_token.empty() ? "no" : "yes",
           g_started ? "yes" : "no");
  ESP_LOGI(TAG, "Starting Home Assistant websocket client");
  if (g_started) return;
  if (g_url.empty() || g_token.empty()) {
    ESP_LOGI(TAG,
             "Home Assistant websocket not configured; live entity updates "
             "disabled");
    return;
  }

  if (!allocate_queue_items()) return;

  // Keep the receive buffer in PSRAM for the lifetime of the websocket
  // client. clear() retains this capacity between websocket messages.
  g_rx_buffer.reserve(kMaxMessageBytes);

  bool is_tls = false;
  const std::string uri = build_uri(g_url, &is_tls);

  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri.c_str();
  cfg.transport =
      is_tls ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP;
  cfg.disable_auto_reconnect = false;
  cfg.reconnect_timeout_ms = 10000;
  cfg.network_timeout_ms = 10000;
  cfg.task_stack = 8192;
  cfg.buffer_size = 4096;
  if (is_tls) {
    // Local Home Assistant installs commonly serve wss:// with a
    // self-signed certificate. Skip verification instead of failing to
    // connect; see CONFIG_ESP_TLS_INSECURE /
    // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY in __init__.py.
    cfg.cert_pem = nullptr;
    cfg.skip_cert_common_name_check = true;
  }

  g_client = esp_websocket_client_init(&cfg);
  if (g_client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant websocket client");
    return;
  }
  esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY,
                                ha_ws_event_handler, nullptr);
  const esp_err_t err = esp_websocket_client_start(g_client);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Unable to start Home Assistant websocket client (%s)",
             esp_err_to_name(err));
    return;
  }
  g_started = true;
  ESP_LOGI(TAG, "Home Assistant websocket client starting: %s (%s)",
           uri.c_str(), is_tls ? "wss" : "ws");
}

void ha_ws_client_loop() {
  if (!g_started || g_consume_item == nullptr) return;
  QueueHandle_t queue = update_queue();
  if (queue == nullptr) {
    ESP_LOGE(TAG, "Unable to process Home Assistant states: queue unavailable");
    return;
  }

  // Bound work per loop() call so a burst of updates cannot stall the
  // ESPHome main loop.
  for (int i = 0; i < 8 && xQueueReceive(queue, g_consume_item, 0) == pdTRUE;
       i++) {
    ESP_LOGD(TAG, "Consuming Home Assistant state (%u bytes)",
             static_cast<unsigned>(g_consume_item->length));
    JsonDocument state_doc(&g_psram_json_allocator);
    const DeserializationError err = deserializeJson(
        state_doc, g_consume_item->data, g_consume_item->length);
    if (err) {
      ESP_LOGW(TAG, "Failed to parse queued Home Assistant state (%s)",
               err.c_str());
      continue;
    }
    apply_ha_entity_state(state_doc);
  }
}

}  // namespace web_admin_local
