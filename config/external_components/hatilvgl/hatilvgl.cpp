#include "hatilvgl.h"

#include <ArduinoJson.h>
#include <atomic>
#include <cstdio>
#include "esphome/components/spiffs/spiffs.h"

namespace web_admin_local {

namespace {
std::atomic<bool> g_api_connected{false};
esphome::light::LightState *g_backlight = nullptr;
esphome::number::Number *g_timeout = nullptr;
HATiFolderSelect *g_folder_select = nullptr;
std::atomic<int> g_requested_folder{-1};
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
