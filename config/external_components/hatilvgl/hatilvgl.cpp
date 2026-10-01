#include "hatilvgl.h"

#include <ArduinoJson.h>
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <vector>
#include <lvgl.h>
#include "esphome/components/spiffs/spiffs.h"
#include "../hatifonts/mdi_icons.h"

namespace web_admin_local {

namespace {
std::atomic<bool> g_api_connected{false};
esphome::light::LightState *g_backlight = nullptr;
esphome::number::Number *g_timeout = nullptr;
HATiFolderSelect *g_folder_select = nullptr;
std::atomic<int> g_requested_folder{-1};
std::vector<lv_obj_t *> g_battery_labels;
std::vector<lv_obj_t *> g_battery_icons;
float g_battery_level = NAN;
}

void HATiFolderSelect::configure_folders() {
  folder_count_ = 0;
  if (esphome::spiffs::ensure_mounted()) {
    FILE *file = fopen("/spiffs/t_folders.json", "rb");
    if (file != nullptr) {
      fseek(file, 0, SEEK_END);
      const long size = ftell(file);
      rewind(file);
      if (size > 0 && size < 8192) {
        std::string raw(static_cast<size_t>(size), '\0');
        if (fread(raw.data(), 1, raw.size(), file) == raw.size()) {
          JsonDocument document;
          if (deserializeJson(document, raw) == DeserializationError::Ok &&
              document["folders"].is<JsonArray>()) {
            for (JsonObject folder : document["folders"].as<JsonArray>()) {
              if (folder_count_ >= folder_options_.size()) break;
              const int id = folder["id"] | -1;
              if (id < 0 || id > 9) continue;
              const char *name = folder["name"] | "";
              folder_ids_[folder_count_] = id;
              folder_options_[folder_count_] =
                  name[0] != '\0'
                      ? name
                      : (id == 0 ? "Home"
                                 : "Folder " + std::to_string(id + 1));
              ++folder_count_;
            }
          }
        }
      }
      fclose(file);
    }
  }

  if (folder_count_ == 0) {
    for (int id = 0; id <= 9; ++id) {
      folder_ids_[folder_count_] = id;
      folder_options_[folder_count_] =
          id == 0 ? "Home" : "Folder " + std::to_string(id + 1);
      ++folder_count_;
    }
  }

  esphome::FixedVector<const char *> options;
  options.init(folder_count_);
  for (size_t index = 0; index < folder_count_; ++index) {
    options.push_back(folder_options_[index].c_str());
  }
  traits.set_options(options);
}

void HATiFolderSelect::control(size_t index) {
  if (index >= folder_count_) {
    ESP_LOGW("hatilvgl", "Invalid displayed folder option index: %u",
             static_cast<unsigned>(index));
    return;
  }
  g_requested_folder.store(folder_ids_[index], std::memory_order_release);
}

void HATiFolderSelect::publish_folder(int folder_id) {
  for (size_t index = 0; index < folder_count_; ++index) {
    if (folder_ids_[index] == folder_id) {
      publish_state(index);
      return;
    }
  }
}

void hatilvgl_publish_displayed_folder(int folder_id) {
  if (g_folder_select != nullptr) g_folder_select->publish_folder(folder_id);
}

void hatilvgl_set_api_connected(bool connected) {
  g_api_connected.store(connected, std::memory_order_release);
}

bool hatilvgl_is_api_connected() {
  return g_api_connected.load(std::memory_order_acquire);
}

void hatilvgl_update_home_assistant_credentials(const std::string &url,
                                                const std::string &token) {
  set_home_assistant_credentials(url, token);
  ha_ws_client_configure(url, token);
  ha_ws_client_reconfigure();
}

void settings_set_display_controls(esphome::light::LightState *backlight,
                                   esphome::number::Number *timeout) {
  g_backlight = backlight;
  g_timeout = timeout;
}

void settings_set_brightness(float brightness) {
  if (g_backlight == nullptr) {
    ESP_LOGW("hatilvgl", "Display backlight is not configured");
    return;
  }
  g_backlight->make_call().set_brightness(brightness).perform();
}

void settings_set_timeout(float timeout) {
  if (g_timeout == nullptr) {
    ESP_LOGW("hatilvgl", "Display timeout is not configured");
    return;
  }
  g_timeout->make_call().set_value(timeout).perform();
}

void hatilvgl_set_battery_level(float percent) {
  g_battery_level = std::isfinite(percent)
                        ? std::max(0.0f, std::min(100.0f, percent))
                        : NAN;
  char text[12];
  if (std::isfinite(g_battery_level)) {
    std::snprintf(text, sizeof(text), "%d%%",
                  static_cast<int>(std::lround(g_battery_level)));
  } else {
    std::snprintf(text, sizeof(text), "--%%");
  }
  for (auto *label : g_battery_labels) {
    if (label != nullptr && lv_obj_is_valid(label)) {
      lv_label_set_text(label, text);
    }
  }
  const char *icon_name = !std::isfinite(g_battery_level)
                              ? "battery-outline"
                              : g_battery_level > 80.0f
                                    ? "battery-90"
                                    : g_battery_level < 10.0f
                                          ? "battery-alert"
                                          : g_battery_level < 30.0f
                                                ? "battery-30"
                                                : "battery-50";
  const lv_color_t color = !std::isfinite(g_battery_level)
                               ? lv_color_white()
                               : g_battery_level > 80.0f
                                     ? lv_color_make(0x40, 0xC8, 0x60)
                                     : g_battery_level < 10.0f
                                           ? lv_color_make(0xF0, 0x30, 0x30)
                                           : g_battery_level < 30.0f
                                                 ? lv_color_make(0xF0, 0xC0, 0x30)
                                                 : lv_color_white();
  const std::string icon_char = getMdiChar(icon_name);
  for (auto *icon : g_battery_icons) {
    if (icon != nullptr && lv_obj_is_valid(icon)) {
      lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
      lv_obj_set_style_text_color(icon, color, 0);
    }
  }
}

void hatilvgl_register_battery_label(lv_obj_t *label) {
  if (label == nullptr) return;
  g_battery_labels.push_back(label);
  lv_obj_add_event_cb(label, [](lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    auto *label = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    g_battery_labels.erase(
        std::remove(g_battery_labels.begin(), g_battery_labels.end(), label),
        g_battery_labels.end());
  }, LV_EVENT_DELETE, nullptr);
  hatilvgl_set_battery_level(g_battery_level);
}

void hatilvgl_register_battery_icon(lv_obj_t *icon) {
  if (icon == nullptr) return;
  g_battery_icons.push_back(icon);
  lv_obj_add_event_cb(icon, [](lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    auto *icon = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    g_battery_icons.erase(
        std::remove(g_battery_icons.begin(), g_battery_icons.end(), icon),
        g_battery_icons.end());
  }, LV_EVENT_DELETE, nullptr);
  hatilvgl_set_battery_level(g_battery_level);
}

void HATiLvglComponent::setup() {
  ESP_LOGI("hatilvgl", "Calling ha_ws_client_configure");
  ha_ws_client_configure(home_assistant_url_, home_assistant_token_);
  settings_set_display_controls(backlight_, timeout_);
  if (folder_select_ != nullptr) {
    g_folder_select = folder_select_;
    folder_select_->configure_folders();
  }
  renderer_start_after_ = esphome::millis() + 5000;
}

void HATiLvglComponent::loop() {
  if (!hatilvgl_is_api_connected()) return;
  if (esphome::millis() < renderer_start_after_) return;
  if (g_tiles_renderer == nullptr) {
    g_tiles_renderer = new TilesLvglRenderer();
    g_tiles_renderer->setup();
    g_tiles_renderer->show_folder(0);
    ESP_LOGI("hatilvgl", "TilesLvglRenderer created; home page load queued");
  }
  const int requested_folder =
      g_requested_folder.exchange(-1, std::memory_order_acq_rel);
  if (requested_folder >= 0) g_tiles_renderer->show_folder(requested_folder);
  g_tiles_renderer->process_pending_refreshes();
}

}  // namespace web_admin_local
