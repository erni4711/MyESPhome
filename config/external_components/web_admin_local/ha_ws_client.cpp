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
  char temperature[16];
  char condition[32];
  char forecast[512];
  char brightness[8];
  char color_temp[8];
  char red[8];
  char green[8];
  char blue[8];
};

esp_websocket_client_handle_t g_client = nullptr;
std::string g_url;
std::string g_token;
bool g_started = false;
bool g_authenticated = false;
std::string g_rx_buffer;

void log_websocket_message(const char *direction, const char *message, size_t size) {
  // Keep protocol logging useful without flooding the ESPHome log with large
  // get_states responses. Authentication tokens must never be logged.
  constexpr size_t kLogPreviewBytes = 256;
  const size_t preview_size = size < kLogPreviewBytes ? size : kLogPreviewBytes;
  ESP_LOGD(TAG, "WebSocket %s: %u bytes%s: %.*s", direction,
           static_cast<unsigned>(size), size > preview_size ? " (preview)" : "",
           static_cast<int>(preview_size), message);
}

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

void enqueue_update(const char *entity_id, const char *state, const char *unit,
                    const char *temperature, const char *condition,
                    const char *brightness, const char *color_temp,
                    const char *red, const char *green, const char *blue,
                    const char *forecast) {
  HaStateUpdate update{};
  std::strncpy(update.entity_id, entity_id, sizeof(update.entity_id) - 1);
  std::strncpy(update.state, state, sizeof(update.state) - 1);
  std::strncpy(update.unit, unit, sizeof(update.unit) - 1);
  std::strncpy(update.temperature, temperature, sizeof(update.temperature) - 1);
  std::strncpy(update.condition, condition, sizeof(update.condition) - 1);
  std::strncpy(update.forecast, forecast, sizeof(update.forecast) - 1);
  std::strncpy(update.brightness, brightness, sizeof(update.brightness) - 1);
  std::strncpy(update.color_temp, color_temp, sizeof(update.color_temp) - 1);
  std::strncpy(update.red, red, sizeof(update.red) - 1);
  std::strncpy(update.green, green, sizeof(update.green) - 1);
  std::strncpy(update.blue, blue, sizeof(update.blue) - 1);
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
  const char *unit = attributes["native_temperature_unit"] |
                     (attributes["unit_of_measurement"] | "");
  char temperature_buf[16] = {};
  JsonVariant temperature_value = attributes["native_temperature"];
  if (temperature_value.isNull()) temperature_value = attributes["temperature"];
  if (temperature_value.is<const char *>()) {
    std::strncpy(temperature_buf, temperature_value.as<const char *>(),
                 sizeof(temperature_buf) - 1);
  } else if (!temperature_value.isNull()) {
    snprintf(temperature_buf, sizeof(temperature_buf), "%.2f",
             temperature_value.as<float>());
  }
  char condition_buf[32] = {};
  const char *condition = attributes["condition"] | "";
  JsonObject current = attributes["current"];
  if (condition[0] == '\0' && !current.isNull()) {
    condition = current["weather"][0]["description"] | "";
  }
  if (condition[0] == '\0') condition = state;
  std::strncpy(condition_buf, condition, sizeof(condition_buf) - 1);
  if (!current.isNull() && temperature_buf[0] == '\0') {
    JsonVariant current_temperature = current["temp"];
    if (!current_temperature.isNull()) {
      snprintf(temperature_buf, sizeof(temperature_buf), "%.2f",
               current_temperature.as<float>());
    }
  }
  char forecast[512] = {};
  JsonArray daily = attributes["daily"];
  if (!daily.isNull()) {
    int written = 0;
    int day = 0;
    for (JsonObject item : daily) {
      if (day >= 4) break;
      const long dt = item["dt"] | 0;
      const float min_temp = item["temp"]["min"] | item["temp"]["day"] | 0.0f;
      const float max_temp = item["temp"]["max"] | item["temp"]["day"] | 0.0f;
      const char *icon = item["weather"][0]["icon"] | "";
      const int count = snprintf(forecast + written, sizeof(forecast) - written,
                                 "%ld,%.1f,%.1f,%s;", dt, min_temp, max_temp, icon);
      if (count <= 0 || static_cast<size_t>(count) >= sizeof(forecast) - written) break;
      written += count;
      ++day;
    }
  }
  char brightness[8] = {}, color_temp[8] = {}, red[8] = {}, green[8] = {}, blue[8] = {};
  auto attribute_number = [&attributes](const char *name, char *output, size_t output_size) {
    JsonVariant value = attributes[name];
    if (!value.isNull()) snprintf(output, output_size, "%d", value.as<int>());
  };
  attribute_number("brightness", brightness, sizeof(brightness));
  attribute_number("color_temp_kelvin", color_temp, sizeof(color_temp));
  if (attributes["color_temp_kelvin"].isNull()) attribute_number("color_temp", color_temp, sizeof(color_temp));
  JsonArray rgb = attributes["rgb_color"];
  if (!rgb.isNull() && rgb.size() >= 3) {
    snprintf(red, sizeof(red), "%d", rgb[0].as<int>());
    snprintf(green, sizeof(green), "%d", rgb[1].as<int>());
    snprintf(blue, sizeof(blue), "%d", rgb[2].as<int>());
  }
  (void) source;
  enqueue_update(entity_id, state, unit, temperature_buf, condition_buf,
                 brightness, color_temp, red, green, blue, forecast);
}

void handle_json_document(JsonDocument &doc) {
  const char *type = doc["type"] | "";
  // A bare state object (no "type" field) comes from a get_states reply
  // that was streamed as one JSON document per state instead of a single
  // result array. Treat it like any other state update.
  if (type[0] == '\0' && doc["entity_id"].is<const char *>()) {
    handle_new_state(doc.as<JsonObject>(), "stream");
    return;
  }
  if (std::strcmp(type, "auth_required") == 0) {
    // Reply with the configured long-lived access token. Never log it.
    std::string auth = std::string("{\"type\":\"auth\",\"access_token\":\"") + g_token + "\"}";
    ESP_LOGD(TAG, "WebSocket TX: auth request (access token omitted)");
    esp_websocket_client_send_text(g_client, auth.c_str(), static_cast<int>(auth.size()),
                                    pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Sent Home Assistant websocket auth request");
  } else if (std::strcmp(type, "auth_ok") == 0) {
    if (!g_authenticated) {
      g_authenticated = true;
      ESP_LOGI(TAG, "Home Assistant websocket authenticated");
      static const char *kSubscribe =
          "{\"id\":1,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}";
        ESP_LOGD(TAG, "WebSocket TX: subscribe_events (id=1, event_type=state_changed)");
      esp_websocket_client_send_text(g_client, kSubscribe, static_cast<int>(std::strlen(kSubscribe)),
                                      pdMS_TO_TICKS(5000));
      ESP_LOGI(TAG, "Subscribed to Home Assistant state_changed events (id=1)");
      // Best-effort: seed currently displayed widgets with the live state
      // instead of waiting for the next state_changed event.
      ha_ws_client_request_states();
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
      } else if (result.is<JsonObject>() && result["entity_id"].is<const char *>()) {
        // Single-state result: streamed one JSON object per state.
        handle_new_state(result.as<JsonObject>(), "initial");
      }
    }
  }
}

void handle_text_message(const char *message, size_t message_size) {
  log_websocket_message("RX", message, message_size);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, message, message_size);
  if (err) {
    // The fallback needs string operations, so materialize the payload only
    // when the normal JSON parse fails.
    const std::string message_copy(message, message_size);
    // The get_states reply may arrive as a stream of newline-delimited JSON
    // documents (one state object per line) instead of a single result
    // object. Fall back to parsing each line on its own.
    size_t start = 0;
    bool parsed_any = false;
    while (start < message_copy.size()) {
      const size_t newline = message_copy.find('\n', start);
      const size_t end = (newline == std::string::npos) ? message_copy.size() : newline;
      if (end > start) {
        JsonDocument line_doc;
        if (!deserializeJson(line_doc, message_copy.data() + start, end - start)) {
          handle_json_document(line_doc);
          parsed_any = true;
        }
      }
      if (newline == std::string::npos) break;
      start = newline + 1;
    }
    if (!parsed_any) {
      ESP_LOGW(TAG, "Failed to parse Home Assistant websocket message (%s), %u bytes",
               err.c_str(), static_cast<unsigned>(message_size));
    }
    return;
  }
  handle_json_document(doc);
}

void handle_text_message(const std::string &message) {
  handle_text_message(message.data(), message.size());
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

      ESP_LOGD(TAG, "WebSocket RX frame: %u/%u bytes at offset %u",
           static_cast<unsigned>(data->data_len),
           static_cast<unsigned>(data->payload_len),
           static_cast<unsigned>(data->payload_offset));

    
      if (static_cast<size_t>(data->payload_len) > kMaxMessageBytes) {
        if (data->payload_offset + data->data_len >= data->payload_len) g_rx_buffer.clear();
        break;
      }
      if (data->payload_offset == 0 && data->data_len == data->payload_len) {
        handle_text_message(data->data_ptr, static_cast<size_t>(data->data_len));
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
  const std::set<std::string> updated_filter(entity_ids.begin(), entity_ids.end());
  entity_filter() = updated_filter;
  xSemaphoreGive(mtx);
  ESP_LOGI(TAG, "Home Assistant websocket filter now tracks %u entit%s",
           static_cast<unsigned>(entity_ids.size()), entity_ids.size() == 1 ? "y" : "ies");
  for (const auto &entity_id : entity_ids) {
    ESP_LOGI(TAG, "Home Assistant websocket subscribed entity: %s", entity_id.c_str());
  }
}

void ha_ws_client_request_states() {
  if (!g_authenticated || g_client == nullptr) return;
  static const char *kGetStates = "{\"id\":2,\"type\":\"get_states\"}";
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
  if (!g_started) return;
  HaStateUpdate update;
  int discarded = 0;
  while (xQueueReceive(update_queue(), &update, 0) == pdTRUE) {
    ++discarded;
  }
  if (discarded > 0) {
    ESP_LOGD(TAG, "Discarded %d stale Home Assistant state updates", discarded);
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
  ESP_LOGI(TAG, "Home Assistant websocket client starting: %s (%s)",
           uri.c_str(), is_tls ? "wss" : "ws");
}

void ha_ws_client_loop() {
  if (!g_started) return;
  HaStateUpdate update;
  // Bound work per loop() call so a burst of updates cannot stall the
  // ESPHome main loop.
  for (int i = 0; i < 8 && xQueueReceive(update_queue(), &update, 0) == pdTRUE; i++) {
    ESP_LOGD(TAG, "Applying Home Assistant update: %s = %s%s%s",
             update.entity_id, update.state,
             update.unit[0] ? " " : "", update.unit[0] ? update.unit : "");
    if (std::strncmp(update.entity_id, "light.", 6) == 0) {
      apply_ha_light_state(update.entity_id, update.state, update.brightness,
                           update.color_temp, update.red, update.green, update.blue);
    } else if (std::strncmp(update.entity_id, "weather.", 8) == 0) {
      apply_ha_weather_state(update.entity_id, update.state, update.temperature,
                             update.condition, update.unit, update.forecast);
    } else {
      apply_ha_entity_state(update.entity_id, update.state, update.unit);
    }
  }
}

}  // namespace web_admin_local
