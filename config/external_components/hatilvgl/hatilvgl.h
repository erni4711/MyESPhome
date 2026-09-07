#pragma once

#include "ha_ws_client.h"
#include "tiles_lvgl.h"
#include "esphome/core/component.h"

namespace web_admin_local {

void hatilvgl_set_api_connected(bool connected);
bool hatilvgl_is_api_connected();

class HATiLvglComponent : public esphome::Component {
 public:
  void set_home_assistant_url(const char *url) {
    home_assistant_url_ = url ? url : "";
  }

  void set_home_assistant_token(const char *token) {
    home_assistant_token_ = token ? token : "";
  }

  void setup() override {
    ESP_LOGI("hatilvgl", "Calling ha_ws_client_configure");
    ha_ws_client_configure(home_assistant_url_, home_assistant_token_);
  }

  void loop() override {
    if (!hatilvgl_is_api_connected()) return;
    if (g_tiles_renderer == nullptr) {
      g_tiles_renderer = new TilesLvglRenderer();
      g_tiles_renderer->setup();
      g_tiles_renderer->show_folder(0);
      ESP_LOGI("hatilvgl", "TilesLvglRenderer created; home page load queued");
    }
    g_tiles_renderer->process_pending_refreshes();
  }

 protected:
  std::string home_assistant_url_;
  std::string home_assistant_token_;
};

}  // namespace web_admin_local
