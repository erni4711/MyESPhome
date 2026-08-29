#include "ha_ws_client.h"
#include "tiles_lvgl.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <cstring>
#include <set>

static const char *TAG = "ha_ws_client";

namespace web_admin_local {

namespace {

// Guard against buffering pathologically large messages (e.g. a get_states
// reply on a Home Assistant install with hundreds of entities). Anything
// bigger is dropped; subscribed state_changed events keep widgets fresh.
constexpr size_t kMaxMessageBytes = 49152;  // 48 KiB
constexpr size_t kQueueLength = 24;
constexpr int kGetStatesId = 2;

struct HaStateUpdate {
  char entity_id[48];
  char state[40];
  char unit[16];
};

esp_websocket_client_handle_t g_client = nullptr;
std::string g_url;
std::string g_token;
bool g_started = false;
bool g_authenticated = false;
std::string g_rx_buffer;

QueueHandle_t update_queue() {
  static QueueHandle_t queue = xQueueCreate(kQueueLength, sizeof(HaStateUpdate));
  return queue;
}

SemaphoreHandle_t filter_mutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

std::set<std::string> &entity_filter() {
  static std::set<std::string> filter;
  return filter;
}

bool entity_is_interesting(const char *entity_id) {
  SemaphoreHandle_t mtx = filter_mutex();
  xSemaphoreTake(mtx, portMAX_DELAY);
  const bool found = entity_filter().count(entity_id) > 0;
  xSemaphoreGive(mtx);
  return found;
}

void enqueue_update(const char *entity_id, const char *state, const char *unit) {
  HaStateUpdate update{};
  std::strncpy(update.entity_id, entity_id, sizeof(update.entity_id) - 1);
  std::strncpy(update.state, state, sizeof(update.state) - 1);
  std::strncpy(update.unit, unit, sizeof(update.unit) - 1);
  if (xQueueSend(update_queue(), &update, 0) != pdTRUE) {
    ESP_LOGW(TAG, "State update queue full, dropping update for %s", entity_id);
  }
}

// Applies one `new_state` JSON object (from a state_changed event or a
// get_states result entry) to the queue, if its entity id is configured.
void handle_new_state(JsonObject new_state, const char *source) {
  const char *entity_id = new_state["entity_id"] | "";
  if (entity_id[0] == '\0' || !entity_is_interesting(entity_id)) return;
  const char *state = new_state["state"] | "";
  JsonVariant attributes = new_state["attributes"];
  const char *unit = attributes["unit_of_measurement"] | "";
  ESP_LOGI(TAG, "Home Assistant %s state: %s = %s%s%s",
           source, entity_id, state,
           unit[0] ? " " : "", unit[0] ? unit : "");
  enqueue_update(entity_id, state, unit);
}

void handle_text_message(const std::string &message) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, message);
  if (err) {
    ESP_LOGW(TAG, "Failed to parse Home Assistant websocket message (%s), %u bytes",
             err.c_str(), static_cast<unsigned>(message.size()));
    return;
  }

  const char *type = doc["type"] | "";
  if (std::strcmp(type, "auth_required") == 0) {
    // Reply with the configured long-lived access token. Never log it.
    std::string auth = std::string("{\"type\":\"auth\",\"access_token\":\"") + g_token + "\"}";
    esp_websocket_client_send_text(g_client, auth.c_str(), static_cast<int>(auth.size()),
                                    pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Sent Home Assistant websocket auth request");
  } else if (std::strcmp(type, "auth_ok") == 0) {
    if (!g_authenticated) {
      g_authenticated = true;
      ESP_LOGI(TAG, "Home Assistant websocket authenticated");
      static const char *kSubscribe =
          "{\"id\":1,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}";
      esp_websocket_client_send_text(g_client, kSubscribe, static_cast<int>(std::strlen(kSubscribe)),
                                      pdMS_TO_TICKS(5000));
      ESP_LOGI(TAG, "Subscribed to Home Assistant state_changed events (id=1)");
      // Best-effort: seed currently displayed widgets with the live state
      // instead of waiting for the next state_changed event.
      static const char *kGetStates = "{\"id\":2,\"type\":\"get_states\"}";
      esp_websocket_client_send_text(g_client, kGetStates, static_cast<int>(std::strlen(kGetStates)),
                                      pdMS_TO_TICKS(5000));
      ESP_LOGI(TAG, "Requested initial Home Assistant entity states (id=2)");
    }
  } else if (std::strcmp(type, "auth_invalid") == 0) {
    g_authenticated = false;
    ESP_LOGW(TAG, "Home Assistant websocket authentication rejected; check the configured token");
  } else if (std::strcmp(type, "event") == 0) {
    JsonVariant event = doc["event"];
    if (event.is<JsonObject>()) {
      JsonObject event_obj = event.as<JsonObject>();
      const char *event_type = event_obj["event_type"] | "";
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
      }
    }
  }
}

void reset_connection_state() {
  g_authenticated = false;
  g_rx_buffer.clear();
}

void ha_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  (void) handler_args;
  (void) base;
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
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
      if (static_cast<size_t>(data->payload_len) > kMaxMessageBytes) {
        if (data->payload_offset + data->data_len >= data->payload_len) g_rx_buffer.clear();
        break;
      }
      if (data->payload_offset == 0) g_rx_buffer.clear();
      if (data->data_len > 0) g_rx_buffer.append(data->data_ptr, static_cast<size_t>(data->data_len));
      if (data->payload_offset + data->data_len >= data->payload_len) {
        handle_text_message(g_rx_buffer);
        g_rx_buffer.clear();
      }
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
std::string build_uri(const std::string &home_assistant_url, bool *out_is_tls) {
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
  return (is_tls ? std::string("wss://") : std::string("ws://")) + url + "/api/websocket";
}

}  // namespace

void ha_ws_client_configure(const std::string &home_assistant_url, const std::string &home_assistant_token) {
  g_url = home_assistant_url;
  g_token = home_assistant_token;
}

void ha_ws_client_set_entity_filter(const std::vector<std::string> &entity_ids) {
  SemaphoreHandle_t mtx = filter_mutex();
  xSemaphoreTake(mtx, portMAX_DELAY);
  entity_filter() = std::set<std::string>(entity_ids.begin(), entity_ids.end());
  xSemaphoreGive(mtx);
  ESP_LOGI(TAG, "Home Assistant websocket filter now tracks %u entit%s",
           static_cast<unsigned>(entity_ids.size()), entity_ids.size() == 1 ? "y" : "ies");
  for (const auto &entity_id : entity_ids) {
    ESP_LOGI(TAG, "Home Assistant websocket subscribed entity: %s", entity_id.c_str());
  }
}

void ha_ws_client_start() {
  if (g_started) return;
  if (g_url.empty() || g_token.empty()) {
    ESP_LOGI(TAG, "Home Assistant websocket not configured; live entity updates disabled");
    return;
  }

  bool is_tls = false;
  const std::string uri = build_uri(g_url, &is_tls);

  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri.c_str();
  cfg.transport = is_tls ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP;
  cfg.disable_auto_reconnect = false;
  cfg.reconnect_timeout_ms = 10000;
  cfg.network_timeout_ms = 10000;
  cfg.task_stack = 8192;
  cfg.buffer_size = 4096;
  if (is_tls) {
    // Local Home Assistant installs commonly serve wss:// with a
    // self-signed certificate. Skip verification instead of failing to
    // connect; see CONFIG_ESP_TLS_INSECURE / CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    // in __init__.py.
    cfg.cert_pem = nullptr;
    cfg.skip_cert_common_name_check = true;
  }

  g_client = esp_websocket_client_init(&cfg);
  if (g_client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant websocket client");
    return;
  }
  esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY, ha_ws_event_handler, nullptr);
  const esp_err_t err = esp_websocket_client_start(g_client);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Unable to start Home Assistant websocket client (%s)", esp_err_to_name(err));
    return;
  }
  g_started = true;
  ESP_LOGI(TAG, "Home Assistant websocket client starting (%s)", is_tls ? "wss" : "ws");
}

void ha_ws_client_loop() {
  if (!g_started) return;
  HaStateUpdate update;
  // Bound work per loop() call so a burst of updates cannot stall the
  // ESPHome main loop.
  for (int i = 0; i < 8 && xQueueReceive(update_queue(), &update, 0) == pdTRUE; i++) {
    apply_ha_entity_state(update.entity_id, update.state, update.unit);
  }
}

}  // namespace web_admin_local
