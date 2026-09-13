#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include "ha_ws_client.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <driver/jpeg_decode.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include "esphome/components/spiffs/spiffs.h"
static void *media_stbi_malloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void *media_stbi_realloc(void *pointer, size_t size) {
  return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void media_stbi_free(void *pointer) { heap_caps_free(pointer); }
#define STBI_MALLOC(size) media_stbi_malloc(size)
#define STBI_REALLOC(pointer, size) media_stbi_realloc(pointer, size)
#define STBI_FREE(pointer) media_stbi_free(pointer)
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static const char *TAG = "tiles_lvgl";

namespace web_admin_local {







TilesLvglRenderer *g_tiles_renderer = nullptr;
static std::string home_assistant_url;
static std::string home_assistant_token;
static std::vector<std::string> pending_rest_entities;
static size_t next_rest_entity = 0;
float hourly_weather_temperature[48] = {};
long hourly_weather_timestamp[48] = {};
bool hourly_weather_valid[48] = {};

constexpr size_t kMaxRestResponseBytes = 65536;
constexpr size_t kMaxMediaArtworkBytes = 512 * 1024;

struct MediaArtwork {
  lv_image_dsc_t descriptor{};
  uint8_t *pixels = nullptr;
};

static std::unordered_map<lv_obj_t *, MediaArtwork> media_artwork_cache;
void release_media_artwork(lv_obj_t *object);

struct RestResponse {
  char *data = nullptr;
  size_t size = 0;

  RestResponse() {
    data = static_cast<char *>(heap_caps_malloc(
        kMaxRestResponseBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }

  ~RestResponse() { heap_caps_free(data); }

  bool append(const char *chunk, size_t length) {
    if (chunk == nullptr || length > kMaxRestResponseBytes ||
        size > kMaxRestResponseBytes - length)
      return false;
    std::memcpy(data + size, chunk, length);
    size += length;
    return true;
  }

};

esp_err_t ha_state_http_event_handler(esp_http_client_event_t *event) {
  if (event == nullptr || event->user_data == nullptr) return ESP_FAIL;
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
  auto *response = static_cast<RestResponse *>(event->user_data);
  if (!response->append(static_cast<const char *>(event->data),
                        static_cast<size_t>(event->data_len))) {
    ESP_LOGW(TAG, "Home Assistant REST state response is too large");
    return ESP_FAIL;
  }
  return ESP_OK;
}

void schedule_ha_entity_states_rest(const std::vector<std::string> &entity_ids) {
  pending_rest_entities = entity_ids;
  next_rest_entity = 0;
}

void process_one_ha_entity_state_rest() {
  if (home_assistant_url.empty() || home_assistant_token.empty() ||
      next_rest_entity >= pending_rest_entities.size()) {
    return;
  }
  std::string base_url = home_assistant_url;
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  const std::string auth = "Bearer " + home_assistant_token;

  const std::string entity_id = pending_rest_entities[next_rest_entity++];
  if (entity_id.empty()) return;
  const std::string url = base_url + "/api/states/" + entity_id;
  RestResponse response;
  if (response.data == nullptr) {
    ESP_LOGW(TAG, "Unable to allocate REST response buffer in PSRAM for %s",
             entity_id.c_str());
    return;
  }
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 5000;
  config.event_handler = ha_state_http_event_handler;
  config.user_data = &response;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize REST state request for %s",
             entity_id.c_str());
    return;
  }
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Accept", "application/json");
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "REST state request failed for %s (status=%d, error=%s)",
             entity_id.c_str(), status, esp_err_to_name(result));
    return;
  }
  JsonDocument state(ha_psram_json_allocator());
  const DeserializationError error =
      deserializeJson(state, response.data, response.size);
  if (error) {
    ESP_LOGW(TAG, "Invalid REST state response for %s (%s)",
             entity_id.c_str(), error.c_str());
    return;
  }
  apply_ha_entity_state(state);
}

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
  lv_obj_t *entity_icon = nullptr;
  lv_obj_t *gauge_arc = nullptr;
  lv_obj_t *switch_obj = nullptr;
  lv_obj_t *state_label = nullptr;
  lv_obj_t *weather_icon = nullptr;
  lv_obj_t *weather_temperature = nullptr;
  lv_obj_t *weather_condition = nullptr;
  lv_obj_t *weather_forecast[8] = {};
  uint8_t weather_forecast_count = 0;
  lv_obj_t *weather_high_labels[8] = {};
  lv_obj_t *weather_low_labels[8] = {};
  lv_obj_t *weather_precipitation_labels[8] = {};
  lv_obj_t *weather_probability_labels[8] = {};
  lv_obj_t *weather_precipitation_bars[8] = {};
  lv_obj_t *climate_current_temperature = nullptr;
  lv_obj_t *climate_setpoint = nullptr;
  lv_obj_t *climate_mode = nullptr;
  lv_obj_t *climate_icon = nullptr;
  lv_obj_t *media_title = nullptr;
  lv_obj_t *media_subtitle = nullptr;
  lv_obj_t *media_state = nullptr;
  lv_obj_t *media_play_pause = nullptr;
  lv_obj_t *media_icon = nullptr;
  lv_obj_t *media_artwork = nullptr;
  lv_obj_t *media_volume_slider = nullptr;
  lv_obj_t *media_volume_label = nullptr;
  lv_obj_t *media_mute_button = nullptr;
  lv_obj_t *media_position_slider = nullptr;
  lv_obj_t *media_position_label = nullptr;
  lv_obj_t *media_duration_label = nullptr;
  lv_image_dsc_t *media_artwork_dsc = nullptr;
  uint8_t *media_artwork_data = nullptr;
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

float weather_json_number(JsonVariantConst value) {
  if (value.isNull()) return NAN;
  if (value.is<const char *>()) {
    char *end = nullptr;
    const char *text = value.as<const char *>();
    const float result = std::strtof(text ? text : "", &end);
    return end != text ? result : NAN;
  }
  if (value.is<JsonObjectConst>()) {
    const JsonObjectConst object = value.as<JsonObjectConst>();
    for (const char *key : {"1h", "3h", "24h", "amount"}) {
      const float result = weather_json_number(object[key]);
      if (!std::isnan(result)) return result;
    }
    return NAN;
  }
  return value.as<float>();
}

float weather_probability(JsonVariantConst probability_value,
                          JsonVariantConst pop_value) {
  float value = weather_json_number(probability_value);
  if (std::isnan(value)) value = weather_json_number(pop_value);
  if (std::isnan(value)) return 0.0f;
  return value <= 1.0f ? value * 100.0f : value;
}

int weather_rain_bar_height(float precipitation) {
  if (precipitation <= 0.0f) return 0;
  return std::min(42, std::max(4, static_cast<int>(std::lround(precipitation * 4.0f))));
}

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

lv_color_t weather_temperature_color(float temperature) {
  if (temperature < 0.0f) return lv_color_make(0x87, 0xCE, 0xEB);
  if (temperature < 18.0f) {
    return lv_color_make(0x4D, 0xB3, 0xFF);
  }
  if (temperature < 25.0f) return lv_color_white();
  if (temperature < 30.0f) {
    const float ratio = (temperature - 25.0f) / 5.0f;
    return lv_color_make(0xFF, static_cast<uint8_t>(0xE0 - ratio * 0x70),
                         static_cast<uint8_t>(0x30 - ratio * 0x30));
  }
  if (temperature < 40.0f) {
    const float ratio = (temperature - 30.0f) / 10.0f;
    return lv_color_make(static_cast<uint8_t>(0xF4 - ratio * 0x69),
                         static_cast<uint8_t>(0x43 - ratio * 0x43), 0x00);
  }
  return lv_color_make(0x8B, 0x00, 0x00);
}

void update_weather_forecast(lv_obj_t *column, const char *day,
                             const char *icon, float max_temp, float min_temp,
                             float precipitation, float probability,
                             lv_obj_t *high_label = nullptr,
                             lv_obj_t *low_label = nullptr,
                             lv_obj_t *precipitation_label = nullptr,
                             lv_obj_t *probability_label = nullptr,
                             lv_obj_t *precipitation_bar = nullptr) {
  if (!column) return;
  char high[24];
  char low[24];
  snprintf(high, sizeof(high), "%.1f C", max_temp);
  snprintf(low, sizeof(low), "%.1f C", min_temp);
  if (lv_obj_get_child_count(column) >= 6) {
    lv_label_set_text(lv_obj_get_child(column, 0), day);
    lv_label_set_text(lv_obj_get_child(column, 1), icon);
    lv_label_set_text(lv_obj_get_child(column, 2), high);
    lv_label_set_text(lv_obj_get_child(column, 3), low);
    lv_obj_set_style_text_color(lv_obj_get_child(column, 2),
                                weather_temperature_color(max_temp),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lv_obj_get_child(column, 3),
                                weather_temperature_color(min_temp),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    if (high_label) {
      lv_label_set_text(high_label, high);
      lv_obj_set_style_text_color(high_label,
                                  weather_temperature_color(max_temp),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (low_label) {
      lv_label_set_text(low_label, low);
      lv_obj_set_style_text_color(low_label,
                                  weather_temperature_color(min_temp),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    char amount[24];
    char chance[24];
    snprintf(amount, sizeof(amount), "%.1f mm", precipitation);
    snprintf(chance, sizeof(chance), "%.0f %%", probability);
    lv_label_set_text(lv_obj_get_child(column, 4), amount);
    lv_label_set_text(lv_obj_get_child(column, 5), chance);
    if (precipitation_label) lv_label_set_text(precipitation_label, amount);
    if (probability_label) lv_label_set_text(probability_label, chance);
    if (precipitation_bar) {
      lv_obj_set_height(precipitation_bar, weather_rain_bar_height(precipitation));
      lv_obj_align(precipitation_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
  }
}

std::string weather_icon_name(const char *value) {
  const std::string icon = value ? value : "";
  if (icon.rfind("weather-", 0) == 0) return icon;
  if (icon == "01d" || icon == "01n" || icon == "sunny") return "weather-sunny";
  if (icon == "02d" || icon == "02n" || icon == "partlycloudy")
    return "weather-partly-cloudy";
  if (icon == "03d" || icon == "03n" || icon == "04d" || icon == "04n" ||
      icon == "cloudy")
    return "weather-cloudy";
  if (icon == "09d" || icon == "09n" || icon == "10d" || icon == "10n" ||
      icon == "rainy")
    return "weather-rainy";
  if (icon == "11d" || icon == "11n" || icon == "lightning")
    return "weather-lightning";
  if (icon == "13d" || icon == "13n" || icon == "snowy") return "weather-snowy";
  if (icon == "50d" || icon == "50n" || icon == "fog") return "weather-fog";
  return icon;
}

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

void register_ha_entity_icon(const std::string &entity_id, lv_obj_t *icon_label) {
  if (entity_id.empty() || icon_label == nullptr) return;
  SensorWidgetBinding binding;
  binding.entity_icon = icon_label;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  lv_obj_add_event_cb(icon_label, [](lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_DELETE)
      unregister_ha_widget_object(
          static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
  }, LV_EVENT_DELETE, nullptr);
}

void register_ha_climate_widget(const std::string &entity_id,
                                lv_obj_t *current_temperature,
                                lv_obj_t *setpoint,
                                lv_obj_t *mode,
                                lv_obj_t *icon) {
  if (entity_id.empty()) return;
  SensorWidgetBinding binding;
  binding.climate_current_temperature = current_temperature;
  binding.climate_setpoint = setpoint;
  binding.climate_mode = mode;
  binding.climate_icon = icon;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  const auto watch = [](lv_obj_t *object) {
    if (!object) return;
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
      }
    }, LV_EVENT_DELETE, nullptr);
  };
  watch(current_temperature);
  watch(setpoint);
  watch(mode);
  watch(icon);
  ESP_LOGD(TAG, "Registered climate widget for %s", entity_id.c_str());
}

struct MediaArtworkDownload {
  uint8_t *data = nullptr;
  size_t size = 0;

  bool append(const uint8_t *chunk, size_t length) {
    if (chunk == nullptr || length > kMaxMediaArtworkBytes ||
        size > kMaxMediaArtworkBytes - length)
      return false;
    if (data == nullptr) {
      data = static_cast<uint8_t *>(heap_caps_malloc(
          kMaxMediaArtworkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (data == nullptr) return false;
    }
    std::memcpy(data + size, chunk, length);
    size += length;
    return true;
  }

  ~MediaArtworkDownload() { heap_caps_free(data); }
};

esp_err_t media_artwork_http_event(esp_http_client_event_t *event) {
  if (event == nullptr || event->user_data == nullptr ||
      event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    return ESP_OK;
  auto *download = static_cast<MediaArtworkDownload *>(event->user_data);
  return download->append(static_cast<const uint8_t *>(event->data),
                          static_cast<size_t>(event->data_len))
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

void release_media_artwork(lv_obj_t *object) {
  const auto it = media_artwork_cache.find(object);
  if (it == media_artwork_cache.end()) return;
  ESP_LOGD(TAG, "Releasing cached media artwork for image=%p (%ux%u, %u bytes)",
           static_cast<void *>(object),
           static_cast<unsigned>(it->second.descriptor.header.w),
           static_cast<unsigned>(it->second.descriptor.header.h),
           static_cast<unsigned>(it->second.descriptor.data_size));
  heap_caps_free(it->second.pixels);
  media_artwork_cache.erase(it);
}

void register_ha_media_widget(const std::string &entity_id,
                              lv_obj_t *title,
                              lv_obj_t *subtitle,
                              lv_obj_t *state,
                              lv_obj_t *play_pause,
                              lv_obj_t *icon,
                              lv_obj_t *artwork,
                              lv_obj_t *volume_slider,
                              lv_obj_t *volume_label,
                              lv_obj_t *mute_button,
                              lv_obj_t *position_slider,
                              lv_obj_t *position_label,
                              lv_obj_t *duration_label) {
  if (entity_id.empty()) return;
  SensorWidgetBinding binding;
  binding.media_title = title;
  binding.media_subtitle = subtitle;
  binding.media_state = state;
  binding.media_play_pause = play_pause;
  binding.media_icon = icon;
  binding.media_artwork = artwork;
  binding.media_volume_slider = volume_slider;
  binding.media_volume_label = volume_label;
  binding.media_mute_button = mute_button;
  binding.media_position_slider = position_slider;
  binding.media_position_label = position_label;
  binding.media_duration_label = duration_label;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  const auto watch = [](lv_obj_t *object) {
    if (!object) return;
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
      if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        release_media_artwork(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
        unregister_ha_widget_object(
            static_cast<lv_obj_t *>(lv_event_get_current_target(event)));
      }
    }, LV_EVENT_DELETE, nullptr);
  };
  watch(title);
  watch(subtitle);
  watch(state);
  watch(play_pause);
  watch(icon);
  watch(artwork);
  watch(volume_slider);
  watch(volume_label);
  watch(mute_button);
  watch(position_slider);
  watch(position_label);
  watch(duration_label);
  ESP_LOGD(TAG, "Registered media widget for %s", entity_id.c_str());
}

bool update_media_artwork(lv_obj_t *image, const std::string &picture_url) {
  if (image == nullptr) return false;
  ESP_LOGD(TAG, "Media artwork update requested: image=%p url_length=%u",
           static_cast<void *>(image),
           static_cast<unsigned>(picture_url.size()));
  release_media_artwork(image);
  lv_image_set_src(image, nullptr);
  lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
  if (picture_url.empty()) {
    ESP_LOGD(TAG, "No entity_picture supplied; using media icon fallback");
    return false;
  }
  if (home_assistant_url.empty()) {
    ESP_LOGW(TAG, "Cannot load entity_picture: Home Assistant URL is empty");
    return false;
  }

  std::string url = picture_url;
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    std::string base = home_assistant_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!url.empty() && url.front() != '/') url.insert(url.begin(), '/');
    url = base + url;
  }
  MediaArtworkDownload download;
  esp_http_client_config_t http_config = {};
  http_config.url = url.c_str();
  http_config.method = HTTP_METHOD_GET;
  http_config.timeout_ms = 5000;
  http_config.event_handler = media_artwork_http_event;
  http_config.user_data = &download;
  esp_http_client_handle_t client = esp_http_client_init(&http_config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize media artwork request");
    return false;
  }
  ESP_LOGD(TAG, "Downloading media artwork (%s URL, %u byte limit)",
           picture_url.rfind("http", 0) == 0 ? "absolute" : "relative",
           static_cast<unsigned>(kMaxMediaArtworkBytes));
  const std::string auth = "Bearer " + home_assistant_token;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Accept", "image/jpeg");
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300 || download.size == 0) {
    ESP_LOGW(TAG, "Media artwork request failed (status=%d, error=%s)",
             status, esp_err_to_name(result));
    return false;
  }
  ESP_LOGD(TAG, "Media artwork download completed: status=%d bytes=%u",
           status, static_cast<unsigned>(download.size));
  if (download.size >= 4) {
    ESP_LOGD(TAG, "Media artwork response signature=%02X %02X %02X %02X",
             download.data[0], download.data[1], download.data[2], download.data[3]);
  } else {
    ESP_LOGW(TAG, "Media artwork response is too short: %u bytes",
             static_cast<unsigned>(download.size));
  }

  jpeg_decode_picture_info_t info = {};
  const esp_err_t info_result =
      jpeg_decoder_get_info(download.data, download.size, &info);
  const bool hardware_header_valid = info_result == ESP_OK;
  if (info_result != ESP_OK) {
    int software_width = 0;
    int software_height = 0;
    if (!stbi_info_from_memory(download.data, static_cast<int>(download.size),
                               &software_width, &software_height,
                               nullptr) ||
        software_width <= 0 || software_height <= 0) {
      ESP_LOGW(TAG, "Media artwork is not a supported JPEG (%s); software header parsing failed: %s",
               esp_err_to_name(info_result),
               stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
      return false;
    }
    info.width = static_cast<uint32_t>(software_width);
    info.height = static_cast<uint32_t>(software_height);
    ESP_LOGD(TAG, "Hardware JPEG header parsing failed (%s); using software metadata: %ux%u",
             esp_err_to_name(info_result),
             static_cast<unsigned>(info.width),
             static_cast<unsigned>(info.height));
  }
  ESP_LOGD(TAG, "Media artwork JPEG metadata: %ux%u sampling=%d",
           static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
           static_cast<int>(info.sample_method));
  if (info.width == 0 || info.height == 0 || info.width > 512 ||
      info.height > 512) {
    ESP_LOGW(TAG, "Media artwork dimensions exceed 512x512");
    return false;
  }
  uint32_t decoded_width = (info.width + 15U) & ~15U;
  uint32_t decoded_height = (info.height + 15U) & ~15U;
  size_t pixel_bytes =
      static_cast<size_t>(decoded_width) * decoded_height * 2U;
  uint8_t *pixels = static_cast<uint8_t *>(heap_caps_malloc(
      pixel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pixels == nullptr) {
    ESP_LOGW(TAG, "Unable to allocate %u media artwork bytes in PSRAM",
             static_cast<unsigned>(pixel_bytes));
    return false;
  }
  ESP_LOGD(TAG, "Allocated media artwork decode buffer: %u bytes in PSRAM",
           static_cast<unsigned>(pixel_bytes));
  uint32_t output_size = 0;
  esp_err_t decode_result = ESP_ERR_NOT_SUPPORTED;
  if (hardware_header_valid) {
    jpeg_decode_engine_cfg_t engine_config = {};
    engine_config.timeout_ms = 5000;
    jpeg_decoder_handle_t decoder = nullptr;
    const esp_err_t engine_result =
        jpeg_new_decoder_engine(&engine_config, &decoder);
    if (engine_result == ESP_OK) {
      jpeg_decode_cfg_t decode_config = {};
      decode_config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
      decode_config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
      decode_config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
      decode_result = jpeg_decoder_process(
          decoder, &decode_config, download.data, download.size, pixels,
          pixel_bytes, &output_size);
      jpeg_del_decoder_engine(decoder);
    } else {
      decode_result = engine_result;
    }
  }
  if (decode_result != ESP_OK || output_size == 0) {
    heap_caps_free(pixels);
    ESP_LOGW(TAG, "Hardware JPEG decode failed (%s, output=%u); trying software decoder",
             esp_err_to_name(decode_result), static_cast<unsigned>(output_size));
    int software_width = 0;
    int software_height = 0;
    int software_channels = 0;
    stbi_uc *software_pixels = stbi_load_from_memory(
        download.data, static_cast<int>(download.size), &software_width,
        &software_height, &software_channels, 3);
    if (software_pixels == nullptr || software_width <= 0 ||
        software_height <= 0 || software_width > 512 || software_height > 512) {
      if (software_pixels != nullptr) stbi_image_free(software_pixels);
      ESP_LOGW(TAG, "Software JPEG decode failed: %s",
               stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
      return false;
    }
    decoded_width = static_cast<uint32_t>(software_width);
    decoded_height = static_cast<uint32_t>(software_height);
    pixel_bytes = static_cast<size_t>(decoded_width) * decoded_height * 2U;
    pixels = static_cast<uint8_t *>(heap_caps_malloc(
        pixel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
      stbi_image_free(software_pixels);
      ESP_LOGW(TAG, "Unable to allocate software JPEG output in PSRAM");
      return false;
    }
    for (size_t i = 0; i < static_cast<size_t>(software_width) *
                               static_cast<size_t>(software_height);
         ++i) {
      const uint8_t red = software_pixels[i * 3];
      const uint8_t green = software_pixels[i * 3 + 1];
      const uint8_t blue = software_pixels[i * 3 + 2];
      const uint16_t rgb565 = static_cast<uint16_t>(
          ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
      std::memcpy(pixels + i * 2, &rgb565, sizeof(rgb565));
    }
    stbi_image_free(software_pixels);
    output_size = static_cast<uint32_t>(pixel_bytes);
    ESP_LOGD(TAG, "Software JPEG decode completed: %ux%u output=%u bytes",
             static_cast<unsigned>(decoded_width),
             static_cast<unsigned>(decoded_height),
             static_cast<unsigned>(output_size));
  }
  ESP_LOGD(TAG, "JPEG decode completed: output=%u bytes", 
           static_cast<unsigned>(output_size));

  MediaArtwork artwork;
  artwork.pixels = pixels;
  artwork.descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
  artwork.descriptor.header.w = decoded_width;
  artwork.descriptor.header.h = decoded_height;
  artwork.descriptor.data_size = output_size;
  artwork.descriptor.data = pixels;
  media_artwork_cache.emplace(image, artwork);
  lv_image_set_src(image, &media_artwork_cache.at(image).descriptor);
  const uint32_t target_width = lv_obj_get_width(image);
  const uint32_t target_height = lv_obj_get_height(image);
  if (target_width > 0 && target_height > 0) {
    const uint32_t width_scale =
        (target_width * 256U) / decoded_width;
    const uint32_t height_scale =
        (target_height * 256U) / decoded_height;
    lv_image_set_scale(image, std::min(width_scale, height_scale));
  }
  lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
  ESP_LOGD(TAG, "Displayed media artwork: %ux%u (%u bytes)",
           static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
           static_cast<unsigned>(output_size));
  return true;
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
                                 lv_obj_t **forecast_labels, uint8_t forecast_count,
                                 lv_obj_t **high_labels, lv_obj_t **low_labels,
                                 lv_obj_t **precipitation_labels,
                                 lv_obj_t **probability_labels,
                                 lv_obj_t **precipitation_bars) {
  if (entity_id.empty()) return;
  SensorWidgetBinding binding;
  binding.weather_icon = icon_label;
  binding.weather_temperature = temperature_label;
  binding.weather_condition = condition_label;
  binding.weather_forecast_count = std::min<uint8_t>(forecast_count, 8);
  for (uint8_t i = 0; i < binding.weather_forecast_count; ++i)
    binding.weather_forecast[i] = forecast_labels[i];
  for (uint8_t i = 0; i < binding.weather_forecast_count; ++i) {
    binding.weather_high_labels[i] = high_labels ? high_labels[i] : nullptr;
    binding.weather_low_labels[i] = low_labels ? low_labels[i] : nullptr;
    binding.weather_precipitation_labels[i] =
        precipitation_labels ? precipitation_labels[i] : nullptr;
    binding.weather_probability_labels[i] =
        probability_labels ? probability_labels[i] : nullptr;
    binding.weather_precipitation_bars[i] =
        precipitation_bars ? precipitation_bars[i] : nullptr;
  }
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
  if (entity_id != "sensor.owm_onecall_daily")
    g_sensor_widget_bindings["sensor.owm_onecall_daily"].push_back(binding);
  if (entity_id != "sensor.owm_onecall_hourly")
    g_sensor_widget_bindings["sensor.owm_onecall_hourly"].push_back(binding);
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
  for (uint8_t i = 0; i < binding.weather_forecast_count; ++i) {
    watch(binding.weather_high_labels[i]);
    watch(binding.weather_low_labels[i]);
    watch(binding.weather_precipitation_labels[i]);
    watch(binding.weather_probability_labels[i]);
    watch(binding.weather_precipitation_bars[i]);
  }
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
       binding.entity_icon == object ||
             binding.switch_obj == object || binding.state_label == object ||
             binding.weather_icon == object ||
             binding.weather_temperature == object ||
             binding.weather_condition == object ||
             binding.climate_current_temperature == object ||
             binding.climate_setpoint == object ||
             binding.climate_mode == object ||
             binding.climate_icon == object ||
             binding.media_title == object ||
             binding.media_subtitle == object ||
             binding.media_state == object ||
             binding.media_play_pause == object ||
             binding.media_icon == object ||
             binding.media_artwork == object ||
             binding.media_volume_slider == object ||
             binding.media_volume_label == object ||
             binding.media_mute_button == object ||
             binding.media_position_slider == object ||
             binding.media_position_label == object ||
             binding.media_duration_label == object ||
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

static void apply_entity_icon(const std::string &entity_id, const std::string &icon) {
  if (entity_id.empty() || icon.empty()) return;
  const std::string icon_name = normalizeMdiIconName(icon);
  const std::string icon_char = getMdiChar(icon_name);
  if (icon_char.empty()) return;
  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) return;
    bindings = it->second;
  }
  for (const auto &binding : bindings) {
    if (binding.entity_icon != nullptr)
      lv_label_set_text(binding.entity_icon, icon_char.c_str());
  }
}

void clear_ha_entity_widgets() {
  MutexGuard lock(widget_registry_mutex());
  ESP_LOGD(TAG, "Clearing %u Home Assistant entity bindings",
           static_cast<unsigned>(g_sensor_widget_bindings.size()));
  g_sensor_widget_bindings.clear();
}

void apply_ha_entity_state(const std::string &entity_id, const std::string &state,
                            const std::string &unit,
                            const std::string &icon) {
  (void) unit;  // the unit label is fixed at tile-build time; only the value/gauge live-update.
  apply_entity_icon(entity_id, icon);
  if (entity_id.empty()) {
    ESP_LOGW(TAG, "Ignoring entity state update with empty entity ID");
    return;
  }
  ESP_LOGI(TAG, "Applying Home Assistant state update for %s", entity_id.c_str());
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
  ESP_LOGD(TAG, "Apply %s state update for %s: %s (%u bindings)", is_numeric ? "numeric" : "textual",
           entity_id.c_str(), state.c_str(), static_cast<unsigned>(bindings.size()));
  
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

void apply_ha_entity_state(const JsonDocument &state) {
  const char *entity_id = state["entity_id"] | "";
  if (entity_id[0] == '\0') {
    ESP_LOGW(TAG, "Ignoring Home Assistant state without an entity ID");
    return;
  }
  ESP_LOGD(TAG, "Applying Home Assistant state for %s", entity_id);
  const std::string id(entity_id);
  const std::string value = state["state"] | "";
  const JsonObjectConst attributes = state["attributes"].as<JsonObjectConst>();
  const std::string unit = attributes["native_temperature_unit"] |
                           (attributes["unit_of_measurement"] | "");
  const std::string icon = attributes["icon"] | "";
  apply_entity_icon(id, icon);

  if (id.rfind("weather.", 0) == 0) {
    char temperature[16] = {};
    JsonVariantConst temperature_value = attributes["native_temperature"];
    if (temperature_value.isNull()) temperature_value = attributes["temperature"];
    if (temperature_value.is<const char *>()) {
      snprintf(temperature, sizeof(temperature), "%s",
               temperature_value.as<const char *>());
    } else if (!temperature_value.isNull()) {
      snprintf(temperature, sizeof(temperature), "%.2f",
               temperature_value.as<float>());
    }

    const char *condition = attributes["condition"] | "";
    const JsonObjectConst current = attributes["current"].as<JsonObjectConst>();
    if (condition[0] == '\0' && !current.isNull()) {
      condition = current["weather"][0]["description"] | "";
    }
    if (condition[0] == '\0') condition = value.c_str();

    char forecast[512] = {};
    const JsonArrayConst daily = attributes["daily"].as<JsonArrayConst>();
    size_t written = 0;
    int day = 0;
    for (JsonObjectConst item : daily) {
      if (day++ >= 8) break;
      const float precipitation_probability = weather_probability(
          item["precipitation_probability"], item["pop"]);
      float precipitation = weather_json_number(item["precipitation"]);
      if (std::isnan(precipitation)) precipitation = weather_json_number(item["rain"]);
      if (std::isnan(precipitation)) precipitation = weather_json_number(item["snow"]);
      if (std::isnan(precipitation)) precipitation = 0.0f;
      const int count = snprintf(
          forecast + written, sizeof(forecast) - written,
          "%ld,%.1f,%.1f,%s,%.1f,%.1f;", item["dt"] | 0L,
          item["temp"]["min"] | item["temp"]["day"] | 0.0f,
          item["temp"]["max"] | item["temp"]["day"] | 0.0f,
          item["weather"][0]["icon"] | "", precipitation, precipitation_probability);
      if (count <= 0 || static_cast<size_t>(count) >= sizeof(forecast) - written) break;
      written += static_cast<size_t>(count);
    }
    apply_ha_weather_state(id, value, temperature, condition, unit, forecast);
    return;
  }

  if (id == "sensor.owm_onecall_daily" || id == "sensor.owm_onecall_hourly") {
    const char *array_name =
        id == "sensor.owm_onecall_daily" ? "daily" : "hourly";
    const JsonVariantConst array_value = attributes[array_name];
    if (!array_value.is<JsonArrayConst>()) {
      ESP_LOGW(TAG, "%s response has no '%s' array (attributes=%u)",
               id.c_str(), array_name, static_cast<unsigned>(attributes.size()));
      return;
    }
    const JsonArrayConst forecast = array_value.as<JsonArrayConst>();
    ESP_LOGD(TAG, "%s forecast array contains %u items",
             id.c_str(), static_cast<unsigned>(forecast.size()));
    std::string encoded;
    int count = 0;
    if (id == "sensor.owm_onecall_hourly") {
      for (bool &valid : hourly_weather_valid) valid = false;
    }
    for (JsonObjectConst item : forecast) {
      if (count >= (id == "sensor.owm_onecall_daily" ? 8 : 48)) break;
      const long timestamp = item["dt"] | 0L;
      if (id == "sensor.owm_onecall_hourly") {
        const float temperature = item["temp"] | 0.0f;
        hourly_weather_timestamp[count] = timestamp;
        hourly_weather_temperature[count] = temperature;
        hourly_weather_valid[count] = true;
        if (count < 3 || count == 47) {
          ESP_LOGD(TAG, "%s[%d]: dt=%ld temp=%.1f",
                   id.c_str(), count, timestamp, temperature);
        }
        ++count;
        continue;
      }
      ++count;
      const float min_temp = id == "sensor.owm_onecall_daily"
                                 ? (item["temp"]["min"] | item["temp"]["day"] | 0.0f)
                                 : (item["temp"] | 0.0f);
      const float max_temp = id == "sensor.owm_onecall_daily"
                                 ? (item["temp"]["max"] | item["temp"]["day"] | 0.0f)
                                 : min_temp;
      const char *weather_icon = item["weather"][0]["icon"] | "";
      const float precipitation_probability = weather_probability(
          item["precipitation_probability"], item["pop"]);
      float precipitation = weather_json_number(item["precipitation"]);
      if (std::isnan(precipitation)) precipitation = weather_json_number(item["rain"]);
      if (std::isnan(precipitation)) precipitation = weather_json_number(item["snow"]);
      if (std::isnan(precipitation)) precipitation = 0.0f;
      if (count <= 3) {
        ESP_LOGD(TAG, "%s[%d]: dt=%ld min=%.1f max=%.1f icon=%s pop=%.1f precip=%.1f",
                 id.c_str(), count - 1, timestamp, min_temp, max_temp,
                 weather_icon, precipitation_probability, precipitation);
      }
      char row[96];
      const int written = snprintf(row, sizeof(row), "%ld,%.1f,%.1f,%s,%.1f,%.1f;",
                                   timestamp, min_temp, max_temp, weather_icon,
                                   precipitation, precipitation_probability);
      if (written > 0 && static_cast<size_t>(written) < sizeof(row)) {
        encoded += row;
      }
    }
    if (id == "sensor.owm_onecall_daily") {
      ESP_LOGD(TAG, "%s accepted %d items, encoded forecast length=%u",
               id.c_str(), count, static_cast<unsigned>(encoded.size()));
      apply_ha_weather_forecast_state(id, encoded);
    } else {
      ESP_LOGD(TAG, "%s accepted %d hourly temperatures", id.c_str(), count);
    }
    return;
  }

  if (id.rfind("light.", 0) == 0) {
    auto number_attribute = [&attributes](const char *name) {
      char result[16] = {};
      JsonVariantConst number = attributes[name];
      if (!number.isNull()) snprintf(result, sizeof(result), "%d", number.as<int>());
      return std::string(result);
    };
    const std::string color_temp = attributes["color_temp_kelvin"].isNull()
                                       ? number_attribute("color_temp")
                                       : number_attribute("color_temp_kelvin");
    const JsonArrayConst rgb = attributes["rgb_color"].as<JsonArrayConst>();
    const std::string red = rgb.size() >= 3 ? std::to_string(rgb[0].as<int>()) : "";
    const std::string green = rgb.size() >= 3 ? std::to_string(rgb[1].as<int>()) : "";
    const std::string blue = rgb.size() >= 3 ? std::to_string(rgb[2].as<int>()) : "";
    apply_ha_light_state(id, value, number_attribute("brightness"), color_temp,
                         red, green, blue);
    return;
  }

  if (id.rfind("climate.", 0) == 0) {
    auto attribute_string = [&attributes](const char *name) {
      char result[24] = {};
      JsonVariantConst attribute = attributes[name];
      if (attribute.is<const char *>()) {
        snprintf(result, sizeof(result), "%s", attribute.as<const char *>());
      } else if (!attribute.isNull()) {
        snprintf(result, sizeof(result), "%.1f", attribute.as<float>());
      }
      return std::string(result);
    };
    const std::string current_temperature =
        attribute_string("current_temperature");
    const std::string setpoint = attribute_string("temperature");
    const std::string hvac_mode = attributes["hvac_mode"] | value;
    apply_ha_climate_state(id, current_temperature, setpoint, hvac_mode,
                           unit, icon);
    return;
  }

  if (id.rfind("media_player.", 0) == 0) {
    const std::string media_title = attributes["media_title"] | "";
    const std::string entity_picture = attributes["entity_picture"] | "";
    const std::string artist = attributes["media_artist"] | "";
    const std::string album = attributes["media_album_name"] | "";
    std::string subtitle = artist;
    if (!artist.empty() && !album.empty()) subtitle += " - ";
    subtitle += album;
    apply_ha_media_state(id, value, media_title, subtitle, icon,
                         entity_picture,
                         attributes["volume_level"] | -1.0f,
                         attributes["is_volume_muted"] | false,
                         attributes["media_position"] | -1.0f,
                         attributes["media_duration"] | -1.0f);
    return;
  }

  apply_ha_entity_state(id, value, unit, icon);
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
        const float value = temperature.empty()
                                ? 0.0f
                                : std::strtof(temperature.c_str(), nullptr);
        lv_obj_set_style_text_color(
            binding.weather_temperature,
            temperature.empty() ? lv_color_white()
                                : weather_temperature_color(value),
            LV_PART_MAIN | LV_STATE_DEFAULT);
      }
      int offset = 0;
      for (uint8_t i = 0; i < binding.weather_forecast_count; ++i) {
        long timestamp = 0;
        float min_temp = 0, max_temp = 0;
        char weather_icon[16] = {};
        float precipitation = 0.0f;
        float probability = 0.0f;
        const int parsed = sscanf(forecast.c_str() + offset,
                                  "%ld,%f,%f,%15[^,],%f,%f;",
                                  &timestamp, &min_temp, &max_temp, weather_icon,
                                  &precipitation, &probability);
        if (parsed != 6) {
          ESP_LOGW(TAG, "%s weather forecast entry %u could not be parsed (parsed=%d)",
                   entity_id.c_str(), static_cast<unsigned>(i), parsed);
          break;
        }
        while (forecast[offset] && forecast[offset] != ';') ++offset;
        if (forecast[offset] == ';') ++offset;
        time_t day_time = timestamp;
        struct tm day_tm;
        localtime_r(&day_time, &day_tm);
        char day_name[4] = {};
        strftime(day_name, sizeof(day_name), "%a", &day_tm);
        const std::string forecast_icon =
            getMdiChar(weather_icon_name(weather_icon));
        ESP_LOGD(TAG,
                 "%s forecast[%u]: day=%s min=%.1f max=%.1f precip=%.1f mm probability=%.1f%%",
                 entity_id.c_str(), static_cast<unsigned>(i), day_name,
                 min_temp, max_temp, precipitation, probability);
        update_weather_forecast(binding.weather_forecast[i], day_name,
                                forecast_icon.empty() ? "?" : forecast_icon.c_str(),
                                max_temp, min_temp, precipitation, probability,
                                binding.weather_high_labels[i],
                                binding.weather_low_labels[i],
                                binding.weather_precipitation_labels[i],
                                binding.weather_probability_labels[i],
                                binding.weather_precipitation_bars[i]);
      }
    }
  }

void apply_ha_weather_forecast_state(const std::string &entity_id,
                                     const std::string &forecast) {
  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) {
      ESP_LOGW(TAG, "No weather widget binding found for %s", entity_id.c_str());
      return;
    }
    bindings = it->second;
  }
  ESP_LOGD(TAG, "Applying encoded weather forecast for %s: %u bytes, %u bindings",
           entity_id.c_str(), static_cast<unsigned>(forecast.size()),
           static_cast<unsigned>(bindings.size()));

  for (const auto &binding : bindings) {
    int offset = 0;
    for (uint8_t i = 0; i < binding.weather_forecast_count; ++i) {
      if (offset >= static_cast<int>(forecast.size())) {
        ESP_LOGW(TAG, "%s forecast ended after %u entries",
                 entity_id.c_str(), static_cast<unsigned>(i));
        break;
      }
      long timestamp = 0;
      float min_temp = 0.0f;
      float max_temp = 0.0f;
      char weather_icon[16] = {};
      float precipitation = 0.0f;
      float probability = 0.0f;
      const int parsed = sscanf(forecast.c_str() + offset,
                                "%ld,%f,%f,%15[^,],%f,%f;",
                                &timestamp, &min_temp, &max_temp, weather_icon,
                                &precipitation, &probability);
      if (parsed != 6) {
        ESP_LOGW(TAG, "Unable to parse %s forecast entry %u at offset %d (parsed=%d)",
                 entity_id.c_str(), static_cast<unsigned>(i), offset, parsed);
        break;
      }
      while (forecast[offset] && forecast[offset] != ';') ++offset;
      if (forecast[offset] == ';') ++offset;
      time_t day_time = timestamp;
      struct tm day_tm;
      localtime_r(&day_time, &day_tm);
      char day_name[4] = {};
      strftime(day_name, sizeof(day_name), "%a", &day_tm);
      const std::string forecast_icon =
          getMdiChar(weather_icon_name(weather_icon));
      ESP_LOGD(TAG, "%s forecast[%u]: day=%s min=%.1f max=%.1f icon=%s precip=%.1f probability=%.1f",
               entity_id.c_str(), static_cast<unsigned>(i), day_name,
               min_temp, max_temp, weather_icon, precipitation, probability);
      update_weather_forecast(binding.weather_forecast[i], day_name,
                              forecast_icon.empty() ? "?" : forecast_icon.c_str(),
                              max_temp, min_temp, precipitation, probability,
                              binding.weather_high_labels[i],
                              binding.weather_low_labels[i],
                              binding.weather_precipitation_labels[i],
                              binding.weather_probability_labels[i],
                              binding.weather_precipitation_bars[i]);
    }


  }
}

void apply_ha_weather_forecast_day_state(const JsonDocument &state) {
  const std::string entity_id = state["entity_id"] | "";
  const size_t prefix_length = std::strlen("sensor.owm_onecall_daily_");
  if (entity_id.rfind("sensor.owm_onecall_daily_", 0) != 0 ||
      entity_id.size() <= prefix_length) return;
  const int day = std::atoi(entity_id.c_str() + prefix_length);
  if (day < 0 || day >= 8) return;

  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) return;
    bindings = it->second;
  }
  const JsonObjectConst attributes = state["attributes"].as<JsonObjectConst>();
  auto number = [&attributes](const char *first, const char *second,
                              float fallback) {
    float result = weather_json_number(attributes[first]);
    if (std::isnan(result) && second != nullptr)
      result = weather_json_number(attributes[second]);
    return std::isnan(result) ? fallback : result;
  };
  const std::string state_value = state["state"] | "";
  const float state_temperature = state_value.empty()
                                      ? 0.0f
                                      : std::strtof(state_value.c_str(), nullptr);
  const float high = number(
      "temperature", "temp_high",
      number("temperature_high", "maximum",
             number("max_temp", "temp_max", state_temperature)));
  const float low = number(
      "templow", "temp_low",
      number("temperature_low", "minimum",
             number("min_temp", "temp_min", high)));
  float precipitation = number("precipitation", "rain", NAN);
  if (std::isnan(precipitation)) precipitation = number("snow", "precipitation_amount", 0.0f);
  const float probability = weather_probability(
      attributes["precipitation_probability"], attributes["pop"]);
  std::string icon_name = attributes["icon"] |
                          (attributes["weather"] |
                           (attributes["condition"] | state_value));
  icon_name = normalizeMdiIconName(icon_name);
  const std::string icon = getMdiChar(weather_icon_name(icon_name.c_str()));
  std::string day_name = attributes["day"] |
                         (attributes["weekday"] |
                          (attributes["name"] | ""));
  if (day_name.empty()) {
    const long timestamp = attributes["dt"] | (attributes["timestamp"] | 0L);
    if (timestamp != 0) {
      time_t day_time = timestamp;
      struct tm day_tm;
      localtime_r(&day_time, &day_tm);
      char formatted[4] = {};
      strftime(formatted, sizeof(formatted), "%a", &day_tm);
      day_name = formatted;
    }
  }
  if (day_name.empty()) day_name = std::to_string(day + 1);

  ESP_LOGD(TAG, "%s: day=%d min=%.1f max=%.1f precip=%.1f mm probability=%.1f%%",
           entity_id.c_str(), day, low, high, precipitation, probability);
  for (const auto &binding : bindings) {
    if (day < binding.weather_forecast_count) {
      update_weather_forecast(binding.weather_forecast[day], day_name.c_str(),
                              icon.empty() ? "?" : icon.c_str(), high, low,
                              precipitation, probability,
                              binding.weather_high_labels[day],
                              binding.weather_low_labels[day],
                              binding.weather_precipitation_labels[day],
                              binding.weather_probability_labels[day],
                              binding.weather_precipitation_bars[day]);
    }
  }
}

void apply_ha_climate_state(const std::string &entity_id,
                            const std::string &current_temperature,
                            const std::string &setpoint,
                            const std::string &hvac_mode,
                            const std::string &unit,
                            const std::string &icon) {
  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) return;
    bindings = it->second;
  }
  const std::string suffix = unit.empty() ? "" : " " + unit;
  for (const auto &binding : bindings) {
    if (binding.climate_current_temperature) {
      const std::string text = current_temperature.empty()
                                   ? "--"
                                   : current_temperature + suffix;
      lv_label_set_text(binding.climate_current_temperature, text.c_str());
    }
    if (binding.climate_setpoint) {
      const std::string text = setpoint.empty() ? "--" : setpoint + suffix;
      lv_label_set_text(binding.climate_setpoint, text.c_str());
    }
    if (binding.climate_mode) {
      lv_label_set_text(binding.climate_mode,
                        hvac_mode.empty() ? "--" : hvac_mode.c_str());
    }
    if (binding.climate_icon && !icon.empty()) {
      const std::string icon_name = normalizeMdiIconName(icon);
      const std::string icon_char = getMdiChar(icon_name);
      if (!icon_char.empty()) {
        lv_label_set_text(binding.climate_icon, icon_char.c_str());
      }
    }
  }
}

static std::string format_media_time(float seconds) {
  if (seconds < 0.0f) return "--:--";
  const int total_seconds = std::max(0, static_cast<int>(std::lround(seconds)));
  const int hours = total_seconds / 3600;
  const int minutes = (total_seconds % 3600) / 60;
  const int remainder = total_seconds % 60;
  char text[16];
  if (hours > 0)
    snprintf(text, sizeof(text), "%d:%02d:%02d", hours, minutes, remainder);
  else
    snprintf(text, sizeof(text), "%02d:%02d", minutes, remainder);
  return text;
}

void apply_ha_media_state(const std::string &entity_id,
                          const std::string &state,
                          const std::string &title,
                          const std::string &subtitle,
                          const std::string &icon,
                          const std::string &entity_picture,
                          float volume_level,
                          bool volume_muted,
                          float media_position,
                          float media_duration) {
  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) return;
    bindings = it->second;
  }
  std::string normalized = state;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool playing = normalized == "playing";
  ESP_LOGD(TAG, "Media state update for %s: state=%s title_length=%u "
                "subtitle_length=%u entity_picture_length=%u",
           entity_id.c_str(), state.c_str(),
           static_cast<unsigned>(title.size()),
           static_cast<unsigned>(subtitle.size()),
           static_cast<unsigned>(entity_picture.size()));
  for (const auto &binding : bindings) {
    if (binding.media_title) {
      lv_label_set_text(binding.media_title,
                        title.empty() ? "--" : title.c_str());
    }
    if (binding.media_subtitle) {
      lv_label_set_text(binding.media_subtitle,
                        subtitle.empty() ? "--" : subtitle.c_str());
    }
    if (binding.media_state) {
      const char *display_state = playing ? "Playing" :
                                   normalized == "paused" ? "Paused" :
                                   state.empty() ? "--" : state.c_str();
      lv_label_set_text(binding.media_state, display_state);
    }
    if (binding.media_play_pause) {
      const std::string icon_name = playing ? "pause" : "play";
      const std::string icon_char = getMdiChar(icon_name);
      if (!icon_char.empty()) {
        lv_label_set_text(binding.media_play_pause, icon_char.c_str());
      }
    }
    if (binding.media_icon && !icon.empty()) {
      const std::string icon_char = getMdiChar(normalizeMdiIconName(icon));
      if (!icon_char.empty()) {
        lv_label_set_text(binding.media_icon, icon_char.c_str());
      }
    }
    if (binding.media_artwork) {
      const bool artwork_loaded =
          update_media_artwork(binding.media_artwork, entity_picture);
      if (artwork_loaded) {
        if (binding.media_icon) lv_obj_add_flag(binding.media_icon, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGD(TAG, "Media artwork displayed for %s; hiding fallback icon",
                 entity_id.c_str());
      } else if (binding.media_icon) {
        lv_obj_clear_flag(binding.media_icon, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGD(TAG, "Media artwork absent for %s; showing fallback icon",
                 entity_id.c_str());
      }
    }
    if (binding.media_volume_slider && volume_level >= 0.0f) {
      const int volume_percent = std::max(
          0, std::min(100, static_cast<int>(std::lround(volume_level * 100.0f))));
      lv_slider_set_value(binding.media_volume_slider, volume_percent,
                          LV_ANIM_OFF);
      if (binding.media_volume_label) {
        char volume_text[8];
        snprintf(volume_text, sizeof(volume_text), "%d%%", volume_percent);
        lv_label_set_text(binding.media_volume_label, volume_text);
      }
    }
    if (binding.media_mute_button) {
      const std::string icon_name = volume_muted ? "volume-off" : "volume-high";
      const std::string icon_char = getMdiChar(icon_name);
      if (!icon_char.empty())
        lv_label_set_text(binding.media_mute_button, icon_char.c_str());
    }
    if (binding.media_position_slider) {
      const int duration = std::max(1, static_cast<int>(std::lround(media_duration)));
      const int position = std::max(
          0, std::min(duration, static_cast<int>(std::lround(media_position))));
      if (media_duration >= 0.0f)
        lv_slider_set_range(binding.media_position_slider, 0, duration);
      if (media_position >= 0.0f)
        lv_slider_set_value(binding.media_position_slider, position, LV_ANIM_OFF);
      if (binding.media_position_label)
        lv_label_set_text(binding.media_position_label,
                          format_media_time(media_position).c_str());
      if (binding.media_duration_label)
        lv_label_set_text(binding.media_duration_label,
                          format_media_time(media_duration).c_str());
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
        add_unique(tile.entity_id);
      } else if (tile.type == TILE_SWITCH) {
        add_unique(tile.entity_id);
      } else if (tile.type == TILE_WEATHER) {
        add_unique(tile.entity_id);
        add_unique("sensor.owm_onecall_daily");
        add_unique("sensor.owm_onecall_hourly");
      } else if (tile.type == TILE_MEDIA ||
                 tile.type == TILE_CLIMATE || tile.type == TILE_CAMERA ||
                 tile.type == TILE_COVER) {
        add_unique(tile.entity_id);
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

bool set_home_assistant_climate_temperature(const char *entity_id,
                                            float temperature) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot set climate temperature: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "climate") {
    ESP_LOGW(TAG, "Cannot set climate temperature for non-climate entity: %s",
             entity_id);
    return false;
  }
  if (!std::isfinite(temperature)) {
    ESP_LOGW(TAG, "Cannot set non-finite climate temperature");
    return false;
  }

  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/climate/set_temperature";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant climate client");
    return false;
  }

  const std::string auth = "Bearer " + home_assistant_token;
  char body[160];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"temperature\":%.1f}",
           entity_id, static_cast<double>(temperature));
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Home Assistant climate temperature failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

bool call_home_assistant_media_command(const char *entity_id,
                                       const char *command) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      command == nullptr || command[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot control media player: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "media_player") {
    ESP_LOGW(TAG, "Cannot control non-media-player entity: %s", entity_id);
    return false;
  }
  if (strcmp(command, "media_play_pause") != 0 &&
      strcmp(command, "media_previous_track") != 0 &&
      strcmp(command, "media_next_track") != 0) {
    ESP_LOGW(TAG, "Unsupported media-player command: %s", command);
    return false;
  }

  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/media_player/";
  url += command;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant media client");
    return false;
  }

  const std::string auth = "Bearer " + home_assistant_token;
  char body[160];
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Home Assistant media command failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

bool set_home_assistant_media_volume(const char *entity_id, float volume) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot set media volume: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "media_player") {
    ESP_LOGW(TAG, "Cannot set volume for non-media-player entity: %s", entity_id);
    return false;
  }
  volume = std::max(0.0f, std::min(1.0f, volume));
  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/media_player/volume_set";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize media volume client");
    return false;
  }
  const std::string auth = "Bearer " + home_assistant_token;
  char body[192];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"volume_level\":%.2f}", entity_id,
           static_cast<double>(volume));
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Media volume request failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

bool set_home_assistant_media_mute(const char *entity_id, bool muted) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot set media mute: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "media_player") {
    ESP_LOGW(TAG, "Cannot mute non-media-player entity: %s", entity_id);
    return false;
  }
  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/media_player/volume_mute";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize media mute client");
    return false;
  }
  const std::string auth = "Bearer " + home_assistant_token;
  char body[192];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"is_volume_muted\":%s}", entity_id,
           muted ? "true" : "false");
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Media mute request failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

bool set_home_assistant_media_position(const char *entity_id, float position) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot seek media player: Home Assistant REST API is not configured");
    return false;
  }
  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id ||
      std::string(entity_id, static_cast<size_t>(dot - entity_id)) != "media_player") {
    ESP_LOGW(TAG, "Cannot seek non-media-player entity: %s", entity_id);
    return false;
  }
  position = std::max(0.0f, position);
  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/media_player/media_seek";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  RestResponse response;
  config.event_handler = ha_state_http_event_handler;
  config.user_data = &response;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize media seek client");
    return false;
  }
  const std::string auth = "Bearer " + home_assistant_token;
  char body[192];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"seek_position\":%.1f}", entity_id,
           static_cast<double>(position));
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG,
             "Media seek request failed for %s (status=%d, error=%s, response=%.*s)",
             entity_id, status, esp_err_to_name(result),
             static_cast<int>(response.size), response.data ? response.data : "");
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
    const char *legacy_entity = "";
    switch (d.type) {
      case TILE_SWITCH:  legacy_entity = t["switch_entity"] | ""; break;
      case TILE_WEATHER: legacy_entity = t["weather_entity"] | ""; break;
      case TILE_ENERGY:  legacy_entity = t["energy_entity"] | ""; break;
      case TILE_MEDIA:   legacy_entity = t["media_entity"] | ""; break;
      case TILE_CLIMATE: legacy_entity = t["climate_entity"] | ""; break;
      case TILE_CAMERA:  legacy_entity = t["camera_entity"] | ""; break;
      case TILE_COVER:   legacy_entity = t["cover_entity"] | ""; break;
      default:           legacy_entity = t["sensor_entity"] | ""; break;
    }
    d.entity_id         = t["entity_id"] | legacy_entity;
    d.sensor_unit       = t["sensor_unit"]      | "";
    d.sensor_decimals   = t["sensor_decimals"]  | -1;
    d.sensor_display_mode = t["sensor_display_mode"] | 0;
    d.sensor_gauge_min  = t["sensor_gauge_min"] | 0.0f;
    d.sensor_gauge_max  = t["sensor_gauge_max"] | 100.0f;
    d.switch_style      = t["switch_style"]     | 0;
    d.scene_alias       = t["scene_alias"]      | "";
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
void tile_widget_build_climate(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_camera(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_settings(lv_obj_t *parent, const TileData &tile);

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

  const bool has_explicit_icon =
      !isMdiIconDisabled(tile.icon_name) &&
      !normalizeMdiIconName(tile.icon_name).empty();
  const std::string configured_icon = tile_icon_name(tile);
  // Weather owns its icon placement so the generic tile icon does not
  // overlap the provider title rendered by tile_widget_build_weather().
  if (!configured_icon.empty() && tile.type != TILE_WEATHER) {
    const std::string icon_char = getMdiChar(configured_icon);
    if (!icon_char.empty()) {
      lv_obj_t *icon = lv_label_create(tile_obj);
      lv_label_set_text(icon, icon_char.c_str());
      lv_obj_set_style_text_color(icon, lv_color_white(), 0);
      lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
      lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, 0, 0);
      if (!has_explicit_icon && !tile.entity_id.empty())
        register_ha_entity_icon(tile.entity_id, icon);
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
    case TILE_CLIMATE:  tile_widget_build_climate(tile_obj, tile); break;
    case TILE_CAMERA:   tile_widget_build_camera(tile_obj, tile);  break;
    case TILE_SETTINGS: tile_widget_build_settings(tile_obj, tile); break;
    case TILE_COVER:
    case TILE_ANIMATE:
      {
        const std::string icon = configured_icon;
        const std::string label = tile.title.empty()
          ? (tile.type == TILE_CAMERA ? "Camera" :
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
  const auto entities = collect_configured_ha_entities();
  ha_ws_client_set_entity_filter(entities);
  ha_ws_client_start();
  schedule_ha_entity_states_rest(entities);
}

void TilesLvglRenderer::refresh_folder(int folder_id) {
  if (folder_id < 0 || folder_id > 9) return;
  ha_ws_client_unsubscribe_events();
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
  // their presentation; query only those entities through the REST API.
  const auto entities = collect_configured_ha_entities();
  ESP_LOGI(TAG, "Requesting %zu HA entity states through REST for folder %d",
           entities.size(), folder_id);
  schedule_ha_entity_states_rest(entities);
  ha_ws_client_subscribe_events();
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
  // WebSocket parsing and LVGL state application are owned by HATiLvgl.
  process_one_ha_entity_state_rest();
  ha_ws_client_loop();
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
