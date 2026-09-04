#include "tiles_lvgl.h"
#include "mdi_icons.h"
#include "ha_ws_client.h"
#include <esp_log.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include "esphome/components/spiffs/spiffs.h"

static const char *TAG = "tiles_lvgl";

namespace web_admin_local {

static lv_obj_t *clock_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color,
                             bool shadow) {
  if (shadow) {
    lv_obj_t *shadow_label = lv_label_create(parent);
    lv_label_set_text(shadow_label, text);
    lv_obj_set_style_text_color(shadow_label, lv_color_black(), 0);
    lv_obj_set_style_text_opa(shadow_label, LV_OPA_50, 0);
    lv_obj_set_style_text_font(shadow_label, font, 0);
    lv_obj_align(shadow_label, LV_ALIGN_CENTER, 2, 2);
  }
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_font(label, font, 0);
  return label;
}

static const lv_font_t *clock_font(int raw_size, int fallback) {
  const int size = raw_size >= 96 ? 96 : raw_size >= 80 ? 80 :
                   raw_size >= 72 ? 72 : raw_size >= 64 ? 64 :
                   raw_size >= 56 ? 56 : raw_size >= 48 ? 48 :
                   raw_size >= 40 ? 40 : raw_size >= 32 ? 32 :
                   raw_size >= 28 ? 28 : raw_size >= 24 ? 24 :
                   raw_size >= 20 ? 20 : fallback;
  return ui_font_for_size(static_cast<uint8_t>(size));
}

static void format_clock_time(char *buf, size_t size, const tm &tm_info,
                              int format) {
  if (format == 2) {
    int hour = tm_info.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(buf, size, "%d:%02d %s", hour, tm_info.tm_min,
             tm_info.tm_hour < 12 ? "AM" : "PM");
    return;
  }
  strftime(buf, size, "%H:%M", &tm_info);
}

struct ClockTimerContext {
  lv_obj_t *label;
  lv_obj_t *date_label;
  lv_timer_t *timer;
  int format;
  int date_format;
  bool show_weekday;
};

static void clock_parent_delete_cb(lv_event_t *event) {
  auto *context = static_cast<ClockTimerContext *>(lv_event_get_user_data(event));
  if (context == nullptr) return;
  if (context->timer != nullptr) {
    lv_timer_delete(context->timer);
    context->timer = nullptr;
  }
  delete context;
}

static void format_clock_date(char *buf, size_t size, const tm &tm_info,
                              int format, bool show_weekday) {
  const char *date_format = (format == 2) ? "%m/%d/%Y"
                           : (format == 3) ? "%Y/%m/%d" : "%d.%m.%Y";
  char date[32];
  strftime(date, sizeof(date), date_format, &tm_info);
  if (show_weekday) {
    char weekday[16];
    strftime(weekday, sizeof(weekday), "%A", &tm_info);
    snprintf(buf, size, "%s, %s", weekday, date);
  } else {
    snprintf(buf, size, "%s", date);
  }
}

TilesLvglRenderer *g_tiles_renderer = nullptr;
static std::string home_assistant_url;
static std::string home_assistant_token;

void set_home_assistant_credentials(const std::string &url, const std::string &token) {
  home_assistant_url = url;
  home_assistant_token = token;
}

// ── Home Assistant websocket live entity updates ─────────────────────────
// See tiles_lvgl.h for the public API. Widgets are registered while a
// folder page is built (tile_widget_build_sensor) and forgotten right
// before that page is torn down (build_folder_on_page), so a stale
// websocket update can never touch a destroyed LVGL object.

namespace {

struct SensorWidgetBinding {
  lv_obj_t *value_label = nullptr;
  lv_obj_t *gauge_arc = nullptr;
  lv_obj_t *switch_obj = nullptr;
  lv_obj_t *state_label = nullptr;
  lv_obj_t *weather_icon = nullptr;
  lv_obj_t *weather_temperature = nullptr;
  lv_obj_t *weather_condition = nullptr;
  lv_obj_t *weather_forecast[4] = {};
  uint8_t weather_forecast_count = 0;
  lv_obj_t *light_popup = nullptr;
  lv_obj_t *light_brightness = nullptr;
  lv_obj_t *light_color_temp = nullptr;
  lv_obj_t *light_red = nullptr;
  lv_obj_t *light_green = nullptr;
  lv_obj_t *light_blue = nullptr;
  int decimals = -1;
  float gauge_min = 0.f;
  float gauge_max = 100.f;
};

std::unordered_map<std::string, std::vector<SensorWidgetBinding>> g_sensor_widget_bindings;

SemaphoreHandle_t widget_registry_mutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

struct MutexGuard {
  explicit MutexGuard(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGive(mutex_); }
  SemaphoreHandle_t mutex_;
};

}  // namespace

void register_ha_entity_widget(const std::string &entity_id, lv_obj_t *value_label,
                                lv_obj_t *gauge_arc, int decimals,
                                float gauge_min, float gauge_max) {
  if (entity_id.empty()) {
    ESP_LOGW(TAG, "Ignoring entity widget with empty entity ID");
    return;
  }
  SensorWidgetBinding binding;
  binding.value_label = value_label;
  binding.gauge_arc = gauge_arc;
  binding.decimals = decimals;
  binding.gauge_min = gauge_min;
  binding.gauge_max = gauge_max;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  if (value_label) {
    lv_obj_add_event_cb(value_label, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE)
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
    }, LV_EVENT_DELETE, nullptr);
  }
  if (gauge_arc) {
    lv_obj_add_event_cb(gauge_arc, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE)
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
    }, LV_EVENT_DELETE, nullptr);
  }
  ESP_LOGD(TAG, "Registered entity widget for %s", entity_id.c_str());
}

void register_ha_switch_widget(const std::string &entity_id, lv_obj_t *switch_obj,
                               lv_obj_t *state_label) {
  if (entity_id.empty() || switch_obj == nullptr) {
    ESP_LOGW(TAG, "Ignoring invalid switch widget for entity '%s'", entity_id.c_str());
    return;
  }
  SensorWidgetBinding binding;
  binding.switch_obj = switch_obj;
  binding.state_label = state_label;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  lv_obj_add_event_cb(switch_obj, [](lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_DELETE)
      unregister_ha_widget_object(
          static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
  }, LV_EVENT_DELETE, nullptr);
  if (state_label) {
    lv_obj_add_event_cb(state_label, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE)
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
    }, LV_EVENT_DELETE, nullptr);
  }
  ESP_LOGD(TAG, "Registered switch widget for %s", entity_id.c_str());
}

void register_ha_weather_widget(const std::string &entity_id, lv_obj_t *icon_label,
                                 lv_obj_t *temperature_label, lv_obj_t *condition_label,
                                 lv_obj_t **forecast_labels, uint8_t forecast_count) {
  if (entity_id.empty()) return;
  SensorWidgetBinding binding;
  binding.weather_icon = icon_label;
  binding.weather_temperature = temperature_label;
  binding.weather_condition = condition_label;
  binding.weather_forecast_count = std::min<uint8_t>(forecast_count, 4);
  for (uint8_t i = 0; i < binding.weather_forecast_count; ++i)
    binding.weather_forecast[i] = forecast_labels[i];
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  const auto watch = [](lv_obj_t *object) {
    if (!object) return;
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE)
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
    }, LV_EVENT_DELETE, nullptr);
  };
  watch(icon_label);
  watch(temperature_label);
  watch(condition_label);
  for (uint8_t i = 0; i < binding.weather_forecast_count; ++i)
    watch(binding.weather_forecast[i]);
  ESP_LOGD(TAG, "Registered weather widget for %s", entity_id.c_str());
}

void register_ha_light_popup(const std::string &entity_id, lv_obj_t *popup,
                             lv_obj_t *brightness, lv_obj_t *color_temp,
                             lv_obj_t *red, lv_obj_t *green, lv_obj_t *blue) {
  if (entity_id.empty() || popup == nullptr) {
    ESP_LOGW(TAG, "Ignoring invalid light popup for entity '%s'", entity_id.c_str());
    return;
  }
  SensorWidgetBinding binding;
  binding.light_popup = popup;
  binding.light_brightness = brightness;
  binding.light_color_temp = color_temp;
  binding.light_red = red;
  binding.light_green = green;
  binding.light_blue = blue;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  const auto watch = [](lv_obj_t *object) {
    if (!object) return;
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE)
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
    }, LV_EVENT_DELETE, nullptr);
  };
  watch(popup);
  watch(brightness);
  watch(color_temp);
  watch(red);
  watch(green);
  watch(blue);
  ESP_LOGD(TAG, "Registered light popup for %s", entity_id.c_str());
}

void unregister_ha_light_popup(lv_obj_t *popup) {
  if (popup == nullptr) return;
  MutexGuard lock(widget_registry_mutex());
  for (auto it = g_sensor_widget_bindings.begin(); it != g_sensor_widget_bindings.end();) {
    auto &bindings = it->second;
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                  [popup](const SensorWidgetBinding &binding) {
                                    return binding.light_popup == popup;
                                  }),
                   bindings.end());
    if (bindings.empty()) it = g_sensor_widget_bindings.erase(it);
    else ++it;
  }
}

void unregister_ha_widget_object(lv_obj_t *object) {
  if (object == nullptr) return;
  MutexGuard lock(widget_registry_mutex());
  for (auto it = g_sensor_widget_bindings.begin(); it != g_sensor_widget_bindings.end();) {
    auto &bindings = it->second;
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                  [object](const SensorWidgetBinding &binding) {
      return binding.value_label == object || binding.gauge_arc == object ||
             binding.switch_obj == object || binding.state_label == object ||
             binding.weather_icon == object ||
             binding.weather_temperature == object ||
             binding.weather_condition == object ||
             binding.light_popup == object ||
             binding.light_brightness == object ||
             binding.light_color_temp == object ||
             binding.light_red == object ||
             binding.light_green == object ||
             binding.light_blue == object ||
             std::any_of(std::begin(binding.weather_forecast),
                         std::end(binding.weather_forecast),
                         [object](lv_obj_t *forecast) { return forecast == object; });
    }), bindings.end());
    if (bindings.empty()) it = g_sensor_widget_bindings.erase(it);
    else ++it;
  }
}

void clear_ha_entity_widgets() {
  MutexGuard lock(widget_registry_mutex());
  ESP_LOGD(TAG, "Clearing %u Home Assistant entity bindings",
           static_cast<unsigned>(g_sensor_widget_bindings.size()));
  g_sensor_widget_bindings.clear();
}

void apply_ha_entity_state(const std::string &entity_id, const std::string &state,
                            const std::string &unit) {
  (void) unit;  // the unit label is fixed at tile-build time; only the value/gauge live-update.
  if (entity_id.empty()) {
    ESP_LOGW(TAG, "Ignoring entity state update with empty entity ID");
    return;
  }

  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) {
      ESP_LOGD(TAG, "No widget binding found for %s", entity_id.c_str());
      return;
    }
    bindings = it->second;  // copy out; LVGL calls below happen unlocked
  }
  
  
  char *num_end = nullptr;
  const double numeric_value = strtod(state.c_str(), &num_end);
  const bool is_numeric = num_end != state.c_str() && *num_end == '\0';
  ESP_LOGD(TAG, "Applying %s state update for %s: %s (%u bindings)", entity_id.c_str(),
           state.c_str(), static_cast<unsigned>(bindings.size()));
  
  for (const auto &binding : bindings) {
    if (binding.switch_obj != nullptr) {
      std::string normalized = state;
      std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (normalized == "on" || normalized == "off") {
        const bool is_on = normalized == "on";
        if (is_on) lv_obj_add_state(binding.switch_obj, LV_STATE_CHECKED);
        else lv_obj_clear_state(binding.switch_obj, LV_STATE_CHECKED);
        lv_obj_t *caption = lv_obj_get_child(binding.switch_obj, 0);
        if (caption) lv_label_set_text(caption, is_on ? "ON" : "OFF");
        if (binding.state_label) lv_label_set_text(binding.state_label, is_on ? "ON" : "OFF");
      }
      continue;
    }
    if (binding.value_label != nullptr) {
      char buf[40];
      if (is_numeric && binding.decimals >= 0) {
        snprintf(buf, sizeof(buf), "%.*f", binding.decimals, numeric_value);
      } else {
        snprintf(buf, sizeof(buf), "%s", state.c_str());
      }
      lv_label_set_text(binding.value_label, buf);
    }
    if (binding.gauge_arc != nullptr && is_numeric) {
      const float clamped = std::max(binding.gauge_min, std::min(binding.gauge_max, static_cast<float>(numeric_value)));
      lv_arc_set_value(binding.gauge_arc, static_cast<int>(clamped));
    }
  }
}

void apply_ha_weather_state(const std::string &entity_id, const std::string &state,
                              const std::string &temperature, const std::string &condition,
                              const std::string &unit, const std::string &forecast) {
    std::vector<SensorWidgetBinding> bindings;
    {
      MutexGuard lock(widget_registry_mutex());
      const auto it = g_sensor_widget_bindings.find(entity_id);
      if (it == g_sensor_widget_bindings.end()) return;
      bindings = it->second;
    }
    const std::string weather_condition = condition.empty() ? state : condition;
    const std::string icon_name =
        weather_condition == "sunny" ? "weather-sunny" :
        weather_condition == "clear-night" ? "weather-night" :
        weather_condition == "cloudy" ? "weather-cloudy" :
        weather_condition == "partlycloudy" ? "weather-partly-cloudy" :
        weather_condition == "rainy" ? "weather-rainy" :
        weather_condition == "pouring" ? "weather-pouring" :
        weather_condition == "snowy" ? "weather-snowy" :
        weather_condition == "snowy-rainy" ? "weather-snowy-rainy" :
        weather_condition == "fog" ? "weather-fog" :
        weather_condition == "windy" ? "weather-windy" :
        weather_condition == "hail" ? "weather-hail" :
        weather_condition == "lightning" ? "weather-lightning" :
        weather_condition == "lightning-rainy" ? "weather-lightning-rainy" :
        "weather-partly-cloudy";
    const std::string icon = getMdiChar(icon_name);
    for (const auto &binding : bindings) {
      if (binding.weather_icon && !icon.empty()) lv_label_set_text(binding.weather_icon, icon.c_str());
      if (binding.weather_condition) lv_label_set_text(binding.weather_condition, weather_condition.c_str());
      if (binding.weather_temperature) {
        std::string text = temperature.empty() ? "--" : temperature;
        if (!unit.empty()) text += " " + unit;
        lv_label_set_text(binding.weather_temperature, text.c_str());
      }
      int offset = 0;
      for (uint8_t i = 0; i < binding.weather_forecast_count; ++i) {
        long timestamp = 0;
        float min_temp = 0, max_temp = 0;
        char weather_icon[16] = {};
        const int parsed = sscanf(forecast.c_str() + offset, "%ld,%f,%f,%15[^;];",
                                  &timestamp, &min_temp, &max_temp, weather_icon);
        if (parsed != 4) break;
        while (forecast[offset] && forecast[offset] != ';') ++offset;
        if (forecast[offset] == ';') ++offset;
        time_t day_time = timestamp;
        struct tm day_tm;
        localtime_r(&day_time, &day_tm);
        char day_name[4] = {};
        strftime(day_name, sizeof(day_name), "%a", &day_tm);
        char row[64];
        snprintf(row, sizeof(row), "%s  %.1f/%.1f %s", day_name, min_temp, max_temp,
                 weather_icon);
        if (binding.weather_forecast[i]) lv_label_set_text(binding.weather_forecast[i], row);
      }
    }
  }

void apply_ha_light_state(const std::string &entity_id, const std::string &state,
                              const std::string &brightness, const std::string &color_temp,
                              const std::string &red, const std::string &green,
                              const std::string &blue) {
      std::vector<SensorWidgetBinding> bindings;
      {
        MutexGuard lock(widget_registry_mutex());
        const auto it = g_sensor_widget_bindings.find(entity_id);
        if (it == g_sensor_widget_bindings.end()) return;
        bindings = it->second;
      }
      auto set_slider = [](lv_obj_t *slider, const std::string &value) {
        if (slider && !value.empty()) lv_slider_set_value(slider, std::atoi(value.c_str()), LV_ANIM_OFF);
      };
      for (const auto &binding : bindings) {
        if (!binding.light_popup) continue;
        if (binding.light_brightness && !brightness.empty()) {
          lv_slider_set_value(binding.light_brightness,
                              std::atoi(brightness.c_str()) * 100 / 255, LV_ANIM_OFF);
        }
        if (binding.light_color_temp && !color_temp.empty()) {
          int kelvin = std::atoi(color_temp.c_str());
          if (kelvin > 0 && kelvin < 1000) kelvin = 1000000 / kelvin;
          lv_slider_set_value(binding.light_color_temp, kelvin, LV_ANIM_OFF);
        }
        set_slider(binding.light_red, red);
        set_slider(binding.light_green, green);
        set_slider(binding.light_blue, blue);
      }
    }
std::vector<std::string> collect_configured_ha_entities() {
  std::vector<std::string> entities;
  auto add_unique = [&](const std::string &id) {
    if (id.empty()) return;
    for (const auto &existing : entities) {
      if (existing == id) return;
    }
    entities.push_back(id);
  };
  for (int folder_id = 0; folder_id <= 9; folder_id++) {
    char path[80];
    snprintf(path, sizeof(path), "/spiffs/t_f%d.json", folder_id);
    FILE *probe = fopen(path, "rb");
    if (!probe) continue;
    fclose(probe);
    for (const auto &tile : read_tile_grid_for_lvgl(folder_id)) {
      if (tile.type == TILE_SENSOR || tile.type == TILE_ENERGY) {
        add_unique(tile.sensor_entity);
        add_unique(tile.energy_entity);
      } else if (tile.type == TILE_SWITCH) {
        add_unique(tile.switch_entity.empty() ? tile.sensor_entity : tile.switch_entity);
      } else if (tile.type == TILE_WEATHER) {
        add_unique(tile.weather_entity.empty() ? tile.sensor_entity : tile.weather_entity);
      }
    }
  }
  return entities;
}

bool toggle_home_assistant_entity(const char *entity_id, bool turn_on) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot toggle entity: Home Assistant REST API is not configured");
    return false;
  }

  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id) {
    ESP_LOGW(TAG, "Cannot toggle invalid entity ID: %s", entity_id);
    return false;
  }
  const std::string domain(entity_id, static_cast<size_t>(dot - entity_id));
  if (domain != "switch" && domain != "light" && domain != "input_boolean") {
    ESP_LOGW(TAG, "Cannot toggle unsupported entity: %s", entity_id);
    return false;
  }

  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/" + domain + (turn_on ? "/turn_on" : "/turn_off");

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant REST client");
    return false;
  }

  const std::string auth = "Bearer " + home_assistant_token;
  const std::string body = std::string("{\"entity_id\":\"") + entity_id + "\"}";
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Home Assistant toggle failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

bool set_home_assistant_light_brightness(const char *entity_id, int brightness_pct) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot set brightness: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "light") {
    ESP_LOGW(TAG, "Cannot set brightness for non-light entity: %s", entity_id);
    return false;
  }
  brightness_pct = std::max(0, std::min(100, brightness_pct));
  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/light/turn_on";

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant brightness client");
    return false;
  }
  const std::string auth = "Bearer " + home_assistant_token;
  char body[160];
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"brightness_pct\":%d}",
           entity_id, brightness_pct);
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Home Assistant brightness failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

static bool call_light_turn_on(const char *entity_id, const char *extra_json) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot control light: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "light") {
    ESP_LOGW(TAG, "Cannot control non-light entity: %s", entity_id);
    return false;
  }
  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/light/turn_on";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return false;
  const std::string auth = "Bearer " + home_assistant_token;
  char body[220];
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",%s}", entity_id, extra_json);
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Home Assistant light control failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

bool set_home_assistant_light_color_temp(const char *entity_id, int kelvin) {
  kelvin = std::max(2000, std::min(6500, kelvin));
  char extra[64];
  snprintf(extra, sizeof(extra), "\"color_temp_kelvin\":%d", kelvin);
  return call_light_turn_on(entity_id, extra);
}

bool set_home_assistant_light_rgb(const char *entity_id, int red, int green, int blue) {
  red = std::max(0, std::min(255, red));
  green = std::max(0, std::min(255, green));
  blue = std::max(0, std::min(255, blue));
  char extra[96];
  snprintf(extra, sizeof(extra), "\"rgb_color\":[%d,%d,%d]", red, green, blue);
  return call_light_turn_on(entity_id, extra);
}

namespace {

struct LightPopupContext {
  char entity_id[128];
  lv_obj_t *overlay = nullptr;
  lv_obj_t *brightness = nullptr;
  lv_obj_t *color_temp = nullptr;
  lv_obj_t *red = nullptr;
  lv_obj_t *green = nullptr;
  lv_obj_t *blue = nullptr;
};

void close_light_popup(lv_event_t *e) {
  auto *context = static_cast<LightPopupContext *>(lv_event_get_user_data(e));
  if (context && context->overlay) {
    lv_obj_del(context->overlay);
  }
}

void light_popup_brightness_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
  auto *context = static_cast<LightPopupContext *>(lv_event_get_user_data(e));
  if (!context || !context->brightness) return;
  const int value = lv_slider_get_value(context->brightness);
  if (!set_home_assistant_light_brightness(context->entity_id, value)) {
    lv_slider_set_value(context->brightness, 100, LV_ANIM_OFF);
  }
}

void light_popup_color_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
  auto *context = static_cast<LightPopupContext *>(lv_event_get_user_data(e));
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  if (!context || !slider) return;
  bool ok = true;
  if (slider == context->color_temp) {
    ok = set_home_assistant_light_color_temp(
        context->entity_id, lv_slider_get_value(slider));
  } else if (slider == context->red || slider == context->green || slider == context->blue) {
    ok = set_home_assistant_light_rgb(
        context->entity_id, lv_slider_get_value(context->red),
        lv_slider_get_value(context->green), lv_slider_get_value(context->blue));
  }
  if (!ok) ESP_LOGW(TAG, "Light popup control request was rejected");
}

void light_popup_power_cb(lv_event_t *e) {
  auto *context = static_cast<LightPopupContext *>(lv_event_get_user_data(e));
  lv_obj_t *button = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  lv_obj_t *label = button ? lv_obj_get_child(button, 0) : nullptr;
  if (!context || !label) return;
  const bool turn_on = std::strcmp(lv_label_get_text(label), "On") == 0;
  toggle_home_assistant_entity(context->entity_id, turn_on);
}

}  // namespace

void show_light_popup(const char *entity_id, const char *title) {
  if (entity_id == nullptr || entity_id[0] == '\0') return;
  lv_obj_t *overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *panel = lv_obj_create(overlay);
  lv_obj_set_size(panel, LV_PCT(84), 300);
  lv_obj_set_style_bg_color(panel, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *heading = lv_label_create(panel);
  lv_label_set_text(heading, (title && title[0]) ? title : entity_id);
  lv_obj_set_style_text_color(heading, lv_color_white(), 0);
  lv_obj_set_style_text_font(heading, ui_font_for_size(16), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 8, 8);

  auto *context = new LightPopupContext{};
  std::strncpy(context->entity_id, entity_id, sizeof(context->entity_id) - 1);
  context->entity_id[sizeof(context->entity_id) - 1] = '\0';
  context->overlay = overlay;

  lv_obj_t *slider = lv_slider_create(panel);
  context->brightness = slider;
  lv_obj_t *brightness_label = lv_label_create(panel);
  lv_label_set_text(brightness_label, "Brightness");
  lv_obj_set_style_text_color(brightness_label, lv_color_white(), 0);
  lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 10, 42);
  lv_obj_set_size(slider, LV_PCT(62), 14);
  lv_slider_set_range(slider, 1, 100);
  lv_slider_set_value(slider, 100, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_make(0x3B, 0x82, 0xF6),
                            LV_PART_INDICATOR);
  lv_obj_align(slider, LV_ALIGN_TOP_RIGHT, -10, 44);
  lv_obj_add_event_cb(slider, light_popup_brightness_cb, LV_EVENT_RELEASED, context);

  context->color_temp = lv_slider_create(panel);
  lv_obj_t *temperature_label = lv_label_create(panel);
  lv_label_set_text(temperature_label, "Color temperature");
  lv_obj_set_style_text_color(temperature_label, lv_color_white(), 0);
  lv_obj_align(temperature_label, LV_ALIGN_TOP_LEFT, 10, 68);
  lv_obj_set_size(context->color_temp, LV_PCT(62), 14);
  lv_slider_set_range(context->color_temp, 2000, 6500);
  lv_slider_set_value(context->color_temp, 4000, LV_ANIM_OFF);
  lv_obj_align(context->color_temp, LV_ALIGN_TOP_RIGHT, -10, 72);
  lv_obj_add_event_cb(context->color_temp, light_popup_color_cb, LV_EVENT_RELEASED, context);
  register_ha_light_popup(entity_id, overlay, context->brightness, context->color_temp,
                          context->red, context->green, context->blue);

  lv_obj_t *colors[] = {
      context->red = lv_slider_create(panel),
      context->green = lv_slider_create(panel),
      context->blue = lv_slider_create(panel)};
  const lv_color_t color_values[] = {
      lv_color_make(0xEF, 0x44, 0x44), lv_color_make(0x22, 0xC5, 0x5E),
      lv_color_make(0x3B, 0x82, 0xF6)};
  const char *color_labels[] = {"Red", "Green", "Blue"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *color_label = lv_label_create(panel);
    lv_label_set_text(color_label, color_labels[i]);
    lv_obj_set_style_text_color(color_label, lv_color_white(), 0);
    lv_obj_align(color_label, LV_ALIGN_TOP_LEFT, 10, 88 + i * 28);
    lv_obj_set_size(colors[i], LV_PCT(62), 12);
    lv_slider_set_range(colors[i], 0, 255);
    lv_slider_set_value(colors[i], i == 0 ? 255 : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(colors[i], color_values[i], LV_PART_INDICATOR);
    lv_obj_align(colors[i], LV_ALIGN_TOP_RIGHT, -10, 94 + i * 28);
    lv_obj_add_event_cb(colors[i], light_popup_color_cb, LV_EVENT_RELEASED, context);
  }

  const char *power_labels[] = {"On", "Off"};
  for (int i = 0; i < 2; ++i) {
    lv_obj_t *power = lv_button_create(panel);
    lv_obj_set_size(power, 64, 30);
    lv_obj_align(power, LV_ALIGN_BOTTOM_LEFT, 16 + i * 72, -8);
    lv_obj_t *power_label = lv_label_create(power);
    lv_label_set_text(power_label, power_labels[i]);
    lv_obj_set_style_text_color(power_label, lv_color_white(), 0);
    lv_obj_align(power_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(power, light_popup_power_cb, LV_EVENT_CLICKED, context);
  }

  lv_obj_t *close = lv_button_create(panel);
  lv_obj_set_size(close, 72, 30);
  lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -16, -8);
  lv_obj_t *close_label = lv_label_create(close);
  lv_label_set_text(close_label, "Close");
  lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
  lv_obj_align(close_label, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(close, close_light_popup, LV_EVENT_CLICKED, context);
  lv_obj_add_event_cb(overlay, [](lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
      auto *context = static_cast<LightPopupContext *>(lv_event_get_user_data(e));
      unregister_ha_light_popup(context ? context->overlay : nullptr);
      delete context;
    }
  }, LV_EVENT_DELETE, context);
}

// ── JSON reader ───────────────────────────────────────────────────────────────

std::vector<TileData> read_tile_grid_for_lvgl(int folder_id) {
  std::vector<TileData> result(35);
  // Fill default grid positions
  for (int i = 0; i < 35; i++) {
    result[i].col = i % 7;
    result[i].row = i / 7;
  }

  if (!esphome::spiffs::ensure_mounted()) return result;
  char path[80];
  snprintf(path, sizeof(path), "/spiffs/t_f%d.json", folder_id);
  FILE *f = fopen(path, "rb");
  if (!f) return result;

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz <= 0 || sz > 65536) { fclose(f); return result; }

  std::string raw(static_cast<size_t>(sz), '\0');
  fread(&raw[0], 1, static_cast<size_t>(sz), f);
  fclose(f);

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return result;

  JsonArray arr;
  if (doc.is<JsonArray>())                arr = doc.as<JsonArray>();
  else if (doc["tiles"].is<JsonArray>())  arr = doc["tiles"].as<JsonArray>();
  else return result;

  int idx = 0;
  for (JsonObject t : arr) {
    if (idx >= 35) break;
    TileData &d = result[idx];
    d.type              = t["type"]             | 0;
    d.col               = t["col"]              | (idx % 7);
    d.row               = t["row"]              | (idx / 7);
    d.span_w            = std::max(1, (int)(t["span_w"] | 1));
    d.span_h            = std::max(1, (int)(t["span_h"] | 1));
    d.bg_color          = t["bg_color"]         | 0u;
    d.icon_name         = t["icon_name"]        | "";
    d.title             = t["title"]            | "";
    d.sensor_entity     = t["sensor_entity"]    | "";
    d.sensor_unit       = t["sensor_unit"]      | "";
    d.sensor_decimals   = t["sensor_decimals"]  | -1;
    d.sensor_display_mode = t["sensor_display_mode"] | 0;
    d.sensor_gauge_min  = t["sensor_gauge_min"] | 0.0f;
    d.sensor_gauge_max  = t["sensor_gauge_max"] | 100.0f;
    d.switch_entity     = t["switch_entity"]    | "";
    d.switch_style      = t["switch_style"]     | 0;
    d.scene_alias       = t["scene_alias"]      | "";
    d.weather_entity    = t["weather_entity"]   | "";
    d.energy_entity     = t["energy_entity"]    | "";
    d.media_entity      = t["media_entity"]     | "";
    d.climate_entity    = t["climate_entity"]   | "";
    d.cover_entity      = t["cover_entity"]     | "";
    d.camera_entity     = t["camera_entity"]    | "";
    d.animation_file    = t["animation_file"]   | "";
    d.text_value        = t["text_value"]       | "";
    d.text_value_font   = t["text_value_font"]  | 0;
    if (t["navigate_target"].is<const char *>()) {
      d.navigate_target = atoi(t["navigate_target"].as<const char *>());
    } else {
      d.navigate_target = t["navigate_target"] | 0;
    }
    d.clock_flags       = t["clock_flags"]      | 1;
    if (t["clock_show_time"].is<const char *>() ||
        t["clock_show_date"].is<const char *>()) {
      const bool show_time = (t["clock_show_time"] | "0") == std::string("1");
      const bool show_date = (t["clock_show_date"] | "0") == std::string("1");
      d.clock_flags = (show_time ? 1 : 0) | (show_date ? 2 : 0);
      if (d.clock_flags == 0) d.clock_flags = 1;
    }
    d.clock_time_format = t["clock_time_format"]| 0;
    d.clock_date_format = t["clock_date_format"]| 0;
    d.clock_show_weekday = t["clock_show_weekday"].is<const char *>()
        ? (std::strcmp(t["clock_show_weekday"] | "0", "1") == 0)
        : (t["clock_show_weekday"] | false);
    d.clock_shadow = t["clock_shadow"].is<const char *>()
        ? (std::strcmp(t["clock_shadow"] | "0", "1") == 0)
        : (t["clock_shadow"] | false);
    d.clock_time_alignment = t["clock_time_alignment"] | 1;
    d.clock_date_alignment = t["clock_date_alignment"] | 1;
    d.key_code          = t["key_code"]         | 40;
    d.key_modifier      = t["key_modifier"]     | 20;
    idx++;
  }
  return result;
}

// ── Forward declarations from per-type files ──────────────────────────────────

void tile_widget_build_sensor(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_clock(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_navigate(lv_obj_t *parent, const TileData &tile, int folder_id);
void tile_widget_build_scene(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_energy(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_media(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_text(lv_obj_t *parent, const TileData &tile);

// ── Shared helper: create a label with given text, colour, font size ──────────

lv_obj_t *lvgl_tile_make_label(lv_obj_t *parent, const char *text,
                                lv_color_t color, int font_size,
                                lv_align_t align = LV_ALIGN_CENTER) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text ? text : "");
  lv_obj_set_style_text_color(lbl, color, 0);
  lv_obj_set_style_text_font(lbl, ui_font_for_size(static_cast<uint8_t>(font_size)), 0);
  lv_obj_align(lbl, align, 0, 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(90));
  return lbl;
}

// ── Resolve tile background colour ────────────────────────────────────────────

static lv_color_t tile_bg_color(const TileData &tile) {
  uint32_t rgb = tile.bg_color & 0xFFFFFF;
  if (rgb == 0) {
    // Per-type defaults (matching admin.css preview defaults)
    switch (tile.type) {
      case TILE_SENSOR:   rgb = 0x1E3A5F; break;
      case TILE_SWITCH:   rgb = 0x2E4A1E; break;
      case TILE_SCENE:    rgb = 0x3A2E1E; break;
      case TILE_CLOCK:    rgb = 0x1A1A2E; break;
      case TILE_WEATHER:  rgb = 0x1A3A4A; break;
      case TILE_ENERGY:   rgb = 0x3A2A0A; break;
      case TILE_MEDIA:    rgb = 0x2A1A3A; break;
      case TILE_NAVIGATE: rgb = 0x1A2A3A; break;
      case TILE_CLIMATE:  rgb = 0x2A1A1A; break;
      default:            rgb = 0x353535; break;
    }

  }
  return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static std::string tile_icon_name(const TileData &tile) {
  if (isMdiIconDisabled(tile.icon_name)) return {};
  std::string icon = normalizeMdiIconName(tile.icon_name);
  if (!icon.empty()) return icon;
  switch (tile.type) {
    case TILE_SENSOR:   return "thermometer";
    case TILE_SWITCH:   return "lightbulb";
    case TILE_SCENE:    return "play-circle";
    case TILE_NAVIGATE: return "folder";
    case TILE_CLOCK:    return "clock-outline";
    case TILE_WEATHER:  return "weather-partly-cloudy";
    case TILE_ENERGY:   return "flash";
    case TILE_MEDIA:    return "television";
    case TILE_CLIMATE:  return "thermostat";
    case TILE_CAMERA:   return "video";
    case TILE_COVER:    return "window-shutter";
    case TILE_SETTINGS: return "cog";
    case TILE_BACK:     return "arrow-left";
    case TILE_ANIMATE:  return "animation-outline";
    default:            return {};
  }
}

// ── Build one tile object ─────────────────────────────────────────────────────

static void tile_click_cb(lv_event_t *e) {
  // Navigate to a folder when a NAVIGATE tile is clicked.
  // We store the target folder_id in user_data.
  int target = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_tiles_renderer && target >= 0) {
    g_tiles_renderer->show_folder(target);
  }
}

void TilesLvglRenderer::build_tile(lv_obj_t *page, const TileData &tile) {
  if (tile.type == TILE_EMPTY) return;

  // Clamp layout
  int col    = std::max(0, std::min(tile.col,    geo_.cols - 1));
  int row    = std::max(0, std::min(tile.row,    geo_.rows - 1));
  int span_w = std::max(1, std::min(tile.span_w, geo_.cols - col));
  int span_h = std::max(1, std::min(tile.span_h, geo_.rows - row));
  for (int r = row; r < row + span_h; ++r)
    for (int c = col; c < col + span_w; ++c)
      if (occupied_[r][c]) return;
  for (int r = row; r < row + span_h; ++r)
    for (int c = col; c < col + span_w; ++c)
      occupied_[r][c] = true;

  int x = geo_.cell_x(col);
  int y = geo_.cell_y(row);
  int w = geo_.tile_w(span_w);
  int h = geo_.tile_h(span_h);

  // Outer tile container
  lv_obj_t *tile_obj = lv_button_create(page);
  lv_obj_set_pos(tile_obj, x, y);
  lv_obj_set_size(tile_obj, w, h);
  lv_obj_set_style_radius(tile_obj, 12, 0);
  lv_obj_set_style_bg_color(tile_obj, tile_bg_color(tile), 0);
  const lv_color32_t base = lv_color_to_32(tile_bg_color(tile), LV_OPA_COVER);
  const lv_color_t pressed = lv_color_make(
      static_cast<uint8_t>(std::min<int>(255, base.red + 16)),
      static_cast<uint8_t>(std::min<int>(255, base.green + 16)),
      static_cast<uint8_t>(std::min<int>(255, base.blue + 16)));
  lv_obj_set_style_bg_color(tile_obj, pressed, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(tile_obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(tile_obj, 0, 0);
  lv_obj_set_style_shadow_width(tile_obj, 0, 0);
  lv_obj_set_style_pad_all(tile_obj, 8, 0);
  lv_obj_set_style_clip_corner(tile_obj, true, 0);
  lv_obj_clear_flag(tile_obj, LV_OBJ_FLAG_SCROLLABLE);

  // Make navigate tiles clickable
  if (tile.type == TILE_NAVIGATE && tile.navigate_target >= 0) {
    lv_obj_add_flag(tile_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile_obj, tile_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)tile.navigate_target);
  }

  const std::string configured_icon = tile_icon_name(tile);
  if (!configured_icon.empty()) {
    const std::string icon_char = getMdiChar(configured_icon);
    if (!icon_char.empty()) {
      lv_obj_t *icon = lv_label_create(tile_obj);
      lv_label_set_text(icon, icon_char.c_str());
      lv_obj_set_style_text_color(icon, lv_color_white(), 0);
      lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
      lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
  }

  // Delegate to per-type widget builders
  switch (tile.type) {
    case TILE_SENSOR:
    case TILE_ENERGY:   tile_widget_build_sensor(tile_obj, tile); break;
    case TILE_CLOCK:    tile_widget_build_clock(tile_obj, tile);  break;
    case TILE_SWITCH:   tile_widget_build_switch(tile_obj, tile); break;
    case TILE_NAVIGATE: tile_widget_build_navigate(tile_obj, tile, tile.navigate_target); break;
    case TILE_SCENE:    tile_widget_build_scene(tile_obj, tile);  break;
    case TILE_WEATHER:  tile_widget_build_weather(tile_obj, tile); break;
    case TILE_MEDIA:    tile_widget_build_media(tile_obj, tile);  break;
    case TILE_TEXT:     tile_widget_build_text(tile_obj, tile);   break;
    case TILE_CLIMATE:
    case TILE_CAMERA:
    case TILE_COVER:
    case TILE_ANIMATE:
      {
        const std::string icon = configured_icon;
        const std::string label = tile.title.empty()
          ? (tile.type == TILE_CLIMATE ? "Climate" :
             tile.type == TILE_CAMERA ? "Camera" :
             tile.type == TILE_COVER ? "Cover" : "Animation")
          : tile.title;
        lv_obj_t *value = lv_label_create(tile_obj);
        lv_label_set_text(value, label.c_str());
        lv_obj_set_style_text_color(value, lv_color_white(), 0);
        lv_obj_set_style_text_font(value, ui_font_for_size(20), 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(value, LV_PCT(100));
        lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, 0);
        (void)icon;
      }
      break;
    case TILE_SETTINGS:
    case TILE_BACK:
      {
        lv_obj_t *lbl = lv_label_create(tile_obj);
        const char *fallback = tile.type == TILE_BACK ? "Back" : "Settings";
        lv_label_set_text(lbl, tile.title.empty() ? fallback : tile.title.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, ui_font_for_size(20), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        if (tile.type == TILE_BACK) {
          lv_obj_add_flag(tile_obj, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_add_event_cb(tile_obj, tile_click_cb, LV_EVENT_CLICKED,
                              (void *)(intptr_t)0);
        }
      }
      break;
    default:
      // Unknown type: show type number as text
      {
        char buf[32]; snprintf(buf, sizeof(buf), "Type %d", tile.type);
        lv_obj_t *lbl = lv_label_create(tile_obj);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_make(180,180,180), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
      }
      break;
  }
}

// ── Build a complete folder page ──────────────────────────────────────────────

void TilesLvglRenderer::build_folder_on_page(int folder_id, lv_obj_t *page,
                                              const std::vector<TileData> &tiles) {
  ESP_LOGI(TAG, "Folder %d rebuild begin: page=%p tiles=%u", folder_id,
           static_cast<void *>(page), static_cast<unsigned>(tiles.size()));
  // Forget any Home Assistant websocket widget bindings for the tiles this
  // page currently holds *before* destroying them, so a state update that
  // arrives mid-rebuild can never touch a dangling LVGL object pointer.
  clear_ha_entity_widgets();

  // Remove all existing children
  lv_obj_clean(page);
  ESP_LOGI(TAG, "Folder %d old LVGL children cleaned", folder_id);

  // Dark background
  lv_obj_set_style_bg_color(page, lv_color_make(0x0A, 0x0A, 0x0A), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  memset(occupied_, 0, sizeof(occupied_));

  for (const auto &tile : tiles) {
    build_tile(page, tile);
    // Keep the LVGL object-tree replacement atomic with respect to redraws.
    esp_task_wdt_reset();
  }

  ESP_LOGI(TAG, "Built folder %d with %zu tiles", folder_id,
           std::count_if(tiles.begin(), tiles.end(),
                         [](const TileData &t){ return t.type != TILE_EMPTY; }));
}

// ── Page management ───────────────────────────────────────────────────────────

lv_obj_t *TilesLvglRenderer::get_or_create_page(int folder_id) {
  for (auto &p : pages_) {
    if (p.folder_id == folder_id) return p.page;
  }
  // Do not create a separate LVGL screen here. ESPHome owns the configured
  // screen/page hierarchy; tile folders are rendered on its active screen.
  lv_obj_t *screen = lv_scr_act();
  pages_.push_back({folder_id, screen});
  ESP_LOGI(TAG, "Using ESPHome LVGL screen for folder %d", folder_id);
  return screen;
}

lv_obj_t *TilesLvglRenderer::get_page(int folder_id) const {
  for (const auto &p : pages_) {
    if (p.folder_id == folder_id) return p.page;
  }
  return nullptr;
}

// ── Public interface ──────────────────────────────────────────────────────────

void TilesLvglRenderer::setup() {
  lv_obj_t *screen = lv_scr_act();
  geo_.screen_w = lv_obj_get_width(screen);
  geo_.screen_h = lv_obj_get_height(screen);
  ESP_LOGI(TAG, "TilesLvglRenderer setup: %dx%d grid %dx%d cell %dx%d",
           geo_.screen_w, geo_.screen_h, geo_.cols, geo_.rows,
           geo_.cell_w(), geo_.cell_h());
}

void TilesLvglRenderer::refresh_folder(int folder_id) {
  if (folder_id < 0 || folder_id > 9) return;
  ESP_LOGI(TAG, "Refreshing folder %d", folder_id);
  const auto tiles = read_tile_grid_for_lvgl(folder_id);
  lv_obj_t *page = get_or_create_page(folder_id);
  build_folder_on_page(folder_id, page, tiles);
  // Keep the websocket subscription filter in sync with whatever is
  // currently configured across all folders (cheap: a handful of small
  // SD-card JSON files).
  ha_ws_client_set_entity_filter(collect_configured_ha_entities());
  // Drop updates queued for the old LVGL object tree while the folder was
  // rebuilt. The fresh snapshot below repopulates the new widgets.
  ha_ws_client_discard_pending_states();
  // A saved tile configuration may keep the same entities while changing
  // their presentation; refresh their values after rebuilding the folder.
  ha_ws_client_request_states();
}

void TilesLvglRenderer::request_refresh_folder(int folder_id) {
  if (folder_id < 0 || folder_id > 9) return;
  pending_refresh_mask_.fetch_or(static_cast<uint16_t>(1U << folder_id),
                                 std::memory_order_relaxed);
}

void TilesLvglRenderer::process_pending_refreshes() {
  const uint16_t pending = pending_refresh_mask_.exchange(0, std::memory_order_relaxed);
  for (int folder_id = 0; folder_id <= 9; ++folder_id) {
    if ((pending & static_cast<uint16_t>(1U << folder_id)) != 0) {
      ESP_LOGI(TAG, "Processing pending folder %d (mask=0x%04x)", folder_id,
               static_cast<unsigned>(pending));
      refresh_folder(folder_id);
      // Process at most one folder per loop iteration. A folder rebuild can
      // still be substantial even after yielding between individual tiles.
      const uint16_t remaining = pending & static_cast<uint16_t>(
          ~static_cast<uint16_t>(1U << folder_id));
      pending_refresh_mask_.fetch_or(remaining, std::memory_order_relaxed);
      break;
    }
  }
}

void TilesLvglRenderer::refresh_all() {
  // Defer filesystem access and LVGL construction to loop(), one folder at a
  // time, instead of blocking startup with all folders in one call.
  pending_refresh_mask_.fetch_or(0x03FF, std::memory_order_relaxed);
}

void TilesLvglRenderer::show_folder(int folder_id) {
  if (folder_id < 0 || folder_id > 9) return;
  lv_obj_t *page = get_page(folder_id);
  // The page pointer is the active ESPHome LVGL screen. Queue the rebuild so
  // navigation never performs SPIFFS I/O and widget destruction synchronously
  // from an LVGL event callback.
  request_refresh_folder(folder_id);
  ESP_LOGI(TAG, "%s folder %d", page ? "Refreshing" : "Loading", folder_id);
}

}  // namespace web_admin_local

// ── Tile type widget implementations (inlined from tile_types/) ────────────
// ─── tile_widget_sensor.cpp ───
// Sensor / Energy tile: shows entity value, unit, optional gauge.

namespace web_admin_local {

void tile_widget_build_sensor(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t muted  = lv_color_make(0x8A, 0x8A, 0x8A);

  bool is_energy = (tile.type == TILE_ENERGY);
  const std::string &entity = is_energy ? tile.energy_entity : tile.sensor_entity;

  // Title / entity label at top
  const char *heading = tile.title.empty() ? entity.c_str() : tile.title.c_str();
  lv_obj_t *title_lbl = lv_label_create(parent);
  lv_label_set_text(title_lbl, heading);
  lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title_lbl, LV_PCT(100));
  lv_obj_set_style_text_color(title_lbl, muted, 0);
  lv_obj_set_style_text_font(title_lbl, ui_font_for_size(16), 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  // Value label in the center (placeholder — live value updated by HA bridge)
  char val_buf[32];
  snprintf(val_buf, sizeof(val_buf), "--");
  lv_obj_t *val_lbl = lv_label_create(parent);
  lv_label_set_text(val_lbl, val_buf);
  lv_obj_set_style_text_color(val_lbl, white, 0);
  lv_obj_set_style_text_font(val_lbl, ui_font_for_size(28), 0);
  lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, 0);

  // Unit label below value
  if (!tile.sensor_unit.empty()) {
    lv_obj_t *unit_lbl = lv_label_create(parent);
    lv_label_set_text(unit_lbl, tile.sensor_unit.c_str());
    lv_obj_set_style_text_color(unit_lbl, muted, 0);
    lv_obj_set_style_text_font(unit_lbl, ui_font_for_size(16), 0);
    lv_obj_align(unit_lbl, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
  }

  // Optional gauge arc (display_mode == 1)
  lv_obj_t *arc = nullptr;
  if (tile.sensor_display_mode == 1) {
    arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 60, 60);
    lv_arc_set_range(arc, (int)tile.sensor_gauge_min, (int)tile.sensor_gauge_max);
    lv_arc_set_value(arc, (int)tile.sensor_gauge_min);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, lv_color_make(0x26, 0xA6, 0x9A), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_align(arc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  // Live updates: Home Assistant `state_changed` events (see
  // ha_ws_client.cpp) refresh val_lbl/arc from the ESPHome loop() task.
  if (!entity.empty()) {
    register_ha_entity_widget(entity, val_lbl, arc, tile.sensor_decimals,
                               tile.sensor_gauge_min, tile.sensor_gauge_max);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_clock.cpp ───
// Clock tile: shows HH:MM time and optional date.

namespace web_admin_local {

void tile_widget_build_clock(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  bool show_time = (tile.clock_flags & 1) != 0;
  bool show_date = (tile.clock_flags & 2) != 0;
  ClockTimerContext *timer_context = nullptr;

  // Title (optional)
  if (!tile.title.empty()) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, tile.title.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(lbl, ui_font_for_size(16), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Time HH:MM
  if (show_time) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char tbuf[16];
    format_clock_time(tbuf, sizeof(tbuf), tm_info, tile.clock_time_format);

    // Time font size from key_code (stored as clock time font size, default 40)
    const lv_font_t *font = clock_font(tile.key_code, 40);
    lv_obj_t *time_lbl = clock_label(parent, tbuf, font, white, tile.clock_shadow);
    const lv_text_align_t time_alignment = tile.clock_time_alignment == 0
        ? LV_TEXT_ALIGN_LEFT : tile.clock_time_alignment == 2
        ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
    lv_obj_set_style_text_align(time_lbl, time_alignment, 0);
    int y_offset = show_date ? -12 : 0;
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, y_offset);

    // Register a 1-second timer to keep the display updated
    timer_context = new ClockTimerContext{time_lbl, nullptr, nullptr,
                                          tile.clock_time_format,
                                          tile.clock_date_format, tile.clock_show_weekday};
    timer_context->timer = lv_timer_create([](lv_timer_t *timer) {
      auto *context = static_cast<ClockTimerContext *>(lv_timer_get_user_data(timer));
      if (!context || !context->label || !lv_obj_is_valid(context->label)) {
        if (context != nullptr) {
          context->timer = nullptr;
        }
        lv_timer_delete(timer);
        return;
      }
      time_t n = time(nullptr);
      struct tm tm;
      localtime_r(&n, &tm);
      char b[16];
      format_clock_time(b, sizeof(b), tm, context->format);
      lv_label_set_text(context->label, b);
      if (context->date_label) {
        char date[100];
        format_clock_date(date, sizeof(date), tm, context->date_format,
                          context->show_weekday);
        lv_label_set_text(context->date_label, date);
      }
    }, 1000, timer_context);
    lv_obj_add_event_cb(parent, clock_parent_delete_cb, LV_EVENT_DELETE,
                        timer_context);
  }

  // Date
  if (show_date) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char dbuf[100];
    format_clock_date(dbuf, sizeof(dbuf), tm_info, tile.clock_date_format,
                      tile.clock_show_weekday);

    lv_obj_t *date_lbl = clock_label(parent, dbuf, clock_font(tile.key_modifier, 20),
                                      lv_color_make(0xCC, 0xCC, 0xCC),
                                      tile.clock_shadow);
    const lv_text_align_t date_alignment = tile.clock_date_alignment == 0
        ? LV_TEXT_ALIGN_LEFT : tile.clock_date_alignment == 2
        ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
    lv_obj_set_style_text_align(date_lbl, date_alignment, 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, show_time ? 22 : 0);
    if (timer_context) timer_context->date_label = date_lbl;
  }
}

}  // namespace web_admin_local

// ─── tile_widget_switch.cpp ───
// Switch / Light tile: toggle button or brightness slider.

namespace web_admin_local {

static void switch_toggle_cb(lv_event_t *e) {
  auto *context = static_cast<SwitchToggleContext *>(lv_event_get_user_data(e));
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  if (!sw || context == nullptr) return;
  const bool turn_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (!toggle_home_assistant_entity(context->entity_id, turn_on)) {
    if (turn_on) lv_obj_clear_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  const char *state = lv_obj_has_state(sw, LV_STATE_CHECKED) ? "ON" : "OFF";
  // Keep both the control caption and the footer state in sync.  The state
  // label is passed as user data because the button owns its caption.
  lv_obj_t *caption = lv_obj_get_child(sw, 0);
  if (caption) lv_label_set_text(caption, state);
  if (context->state_label) lv_label_set_text(context->state_label, state);
}

static void switch_popup_cb(lv_event_t *e) {
  auto *context = static_cast<SwitchToggleContext *>(lv_event_get_user_data(e));
  if (context && std::strncmp(context->entity_id, "light.", 6) == 0) {
    show_light_popup(context->entity_id, context->title);
  }
}

void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);
  const std::string &entity = tile.switch_entity.empty()
                              ? tile.sensor_entity : tile.switch_entity;

  // Title label
  const char *heading = tile.title.empty() ? entity.c_str() : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, heading);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, muted, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(16), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *switch_obj = nullptr;
  if (tile.switch_style == 1) {
    // Brightness slider style. Brightness commands are not available in the
    // local Home Assistant REST adapter yet.
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, LV_PCT(85), 14);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(0x3B, 0x82, 0xF6),
                              LV_PART_INDICATOR);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
  } else {
    // Toggle button, matching the HomeTiles standard switch tile.
    switch_obj = lv_button_create(parent);
    lv_obj_set_size(switch_obj, LV_PCT(70), 36);
    lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x2A, 0x2A, 0x2A), 0);
    lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x3B, 0x82, 0xF6),
                              LV_STATE_CHECKED);
    lv_obj_set_style_radius(switch_obj, 8, 0);
    lv_obj_align(switch_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(switch_obj, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_t *caption = lv_label_create(switch_obj);
    lv_label_set_text(caption, "OFF");
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, 0);
  }

  lv_obj_t *state_lbl = lv_label_create(parent);
  lv_label_set_text(state_lbl, "OFF");
  lv_obj_set_style_text_color(state_lbl, muted, 0);
  lv_obj_set_style_text_font(state_lbl, ui_font_for_size(16), 0);
  lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  if (switch_obj != nullptr) {
    auto *context = new SwitchToggleContext{state_lbl, {}, {}};
    std::strncpy(context->entity_id, entity.c_str(), sizeof(context->entity_id) - 1);
    context->entity_id[sizeof(context->entity_id) - 1] = '\0';
    std::strncpy(context->title, tile.title.c_str(), sizeof(context->title) - 1);
    context->title[sizeof(context->title) - 1] = '\0';
    lv_obj_add_event_cb(switch_obj, switch_toggle_cb, LV_EVENT_VALUE_CHANGED, context);
    lv_obj_add_event_cb(switch_obj, switch_popup_cb, LV_EVENT_LONG_PRESSED, context);
    if (!entity.empty()) register_ha_switch_widget(entity, switch_obj, state_lbl);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_navigate.cpp ───
// Navigate tile: tapping navigates to another folder page.

namespace web_admin_local {

void tile_widget_build_navigate(lv_obj_t *parent, const TileData &tile, int /*folder_id*/) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Arrow icon at centre-top
  lv_obj_t *arrow = lv_label_create(parent);
  lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(arrow, lv_color_make(0x26, 0xA6, 0x9A), 0);
  lv_obj_set_style_text_font(arrow, ui_font_for_size(24), 0);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, 0, 0);

  // Folder name
  const char *name = tile.title.empty() ? "Folder" : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, ui_font_for_size(20), 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

  // "Tap to enter" hint
  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "Tap to open");
  lv_obj_set_style_text_color(hint, muted, 0);
  lv_obj_set_style_text_font(hint, ui_font_for_size(16), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

}  // namespace web_admin_local

// ─── tile_widget_scene.cpp ───
// Scene / Script tile: tap-to-trigger action tile.

namespace web_admin_local {

void tile_widget_build_scene(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);

  // Large play icon
  lv_obj_t *icon = lv_label_create(parent);
  lv_label_set_text(icon, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(icon, accent, 0);
  lv_obj_set_style_text_font(icon, ui_font_for_size(28), 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);

  // Scene / script name
  const char *name = tile.title.empty() ? tile.scene_alias.c_str() : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
}

}  // namespace web_admin_local

// ─── tile_widget_weather.cpp ───
// Weather tile: shows condition + temperature.

namespace web_admin_local {

void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();

  const std::string &entity = tile.weather_entity.empty()
                              ? tile.sensor_entity : tile.weather_entity;
  const char *location = tile.title.empty()
      ? (entity.empty() ? "--" : entity.c_str()) : tile.title.c_str();
  lv_obj_t *t = lv_label_create(parent);
  lv_label_set_text(t, location);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_width(t, LV_PCT(70));
  lv_obj_set_style_text_color(t, white, 0);
  lv_obj_set_style_text_font(t, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(t, LV_ALIGN_TOP_RIGHT, 0, 4);

  lv_obj_t *icon = lv_label_create(parent);
  const std::string icon_name = tile_icon_name(tile);
  const std::string icon_char = getMdiChar(
      icon_name.empty() ? "weather-partly-cloudy" : icon_name);
  lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xD5, 0x4F), 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(40), 0);
  lv_obj_align(temp, LV_ALIGN_CENTER, 0, 8);

  lv_obj_t *condition = lv_label_create(parent);
  lv_label_set_text(condition, "--");
  lv_obj_set_style_text_color(condition, white, 0);
  lv_obj_set_style_text_font(condition, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(condition, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(condition, LV_PCT(100));
  lv_obj_align(condition, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_t *forecast[4] = {};
  uint8_t forecast_count = (tile.span_h >= 2 || tile.span_w >= 2) ? 4 : 0;
  for (uint8_t i = 0; i < forecast_count; ++i) {
    forecast[i] = lv_label_create(parent);
    lv_label_set_text(forecast[i], "--");
    lv_obj_set_style_text_color(forecast[i], white, 0);
    lv_obj_set_style_text_font(forecast[i], ui_font_for_size(12), 0);
    lv_obj_align(forecast[i], LV_ALIGN_BOTTOM_LEFT, 0, -static_cast<int>(i * 16));
  }
  if (!entity.empty()) {
    register_ha_weather_widget(entity, icon, temp, condition, forecast, forecast_count);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_media.cpp ───
// Media player tile: shows track info + playback controls.

namespace web_admin_local {

void tile_widget_build_media(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t muted  = lv_color_make(0x8A, 0x8A, 0x8A);
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);

  // Title / player name at top
  const char *name = tile.title.empty()
                   ? (tile.media_entity.empty() ? "Media" : tile.media_entity.c_str())
                   : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, name);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, muted, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(16), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  // "Now playing" placeholder
  lv_obj_t *track = lv_label_create(parent);
  lv_label_set_text(track, "--");
  lv_obj_set_style_text_color(track, white, 0);
  lv_obj_set_style_text_font(track, ui_font_for_size(16), 0);
  lv_label_set_long_mode(track, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(track, LV_PCT(100));
  lv_obj_align(track, LV_ALIGN_CENTER, 0, -8);

  // Playback buttons row
  lv_obj_t *btn_row = lv_obj_create(parent);
  lv_obj_set_size(btn_row, LV_PCT(100), 36);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_set_style_pad_all(btn_row, 0, 0);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

  const char *btns[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
  for (auto *sym : btns) {
    lv_obj_t *btn = lv_button_create(btn_row);
    lv_obj_set_size(btn, 36, 32);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_bg_color(btn, accent, LV_STATE_PRESSED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, white, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_text.cpp ───
// Text tile: displays a static text value.

namespace web_admin_local {

void tile_widget_build_text(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Optional small title at top
  if (!tile.title.empty()) {
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, tile.title.c_str());
    lv_obj_set_style_text_color(t, muted, 0);
    lv_obj_set_style_text_font(t, ui_font_for_size(16), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Choose font based on text_value_font index (0=default 28, 1=36, etc.)
  const lv_font_t *font;
  switch (tile.text_value_font) {
    case 1:  font = &ui_font_20_semibold; break;
    case 2:  font = ui_font_for_size(28); break;
    default: font = ui_font_for_size(20); break;
  }

  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, tile.text_value.empty() ? "--" : tile.text_value.c_str());
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, tile.title.empty() ? 0 : 8);
}

}  // namespace web_admin_local

// ─── tile_widget_energy.cpp ───
// Energy / power tile — re-uses sensor layout.
// This file is intentionally minimal: build_sensor already handles TILE_ENERGY
// because the TilesLvglRenderer dispatches both TILE_SENSOR and TILE_ENERGY to
// tile_widget_build_sensor().  This file provides the stub forward declaration
// so that the linker is satisfied.

// tile_widget_build_sensor declared in tile_widget_sensor.cpp
// No separate implementation needed for energy — handled by shared sensor widget.
