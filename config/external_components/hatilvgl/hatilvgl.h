#pragma once

#include <array>
#include <string>

#include "ha_ws_client.h"
#include "tiles_lvgl.h"
#include "esphome/core/component.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"

namespace web_admin_local {

void hatilvgl_set_api_connected(bool connected);
bool hatilvgl_is_api_connected();
void hatilvgl_update_home_assistant_credentials(const std::string &url,
                                                const std::string &token);
void settings_set_display_controls(esphome::light::LightState *backlight,
                                   esphome::number::Number *timeout);
void settings_set_brightness(float brightness);
void settings_set_timeout(float timeout);
void hatilvgl_publish_displayed_folder(int folder_id);

class HATiFolderSelect : public esphome::select::Select {
 public:
  void configure_folders();
  void publish_folder(int folder_id);

 protected:
  void control(size_t index) override;

  std::array<std::string, 10> folder_options_;
  std::array<int, 10> folder_ids_{};
  size_t folder_count_ = 0;
};

class HATiLvglComponent : public esphome::Component {
 public:
  void set_home_assistant_url(const char *url) {
    home_assistant_url_ = url ? url : "";
  }

  void set_home_assistant_token(const char *token) {
    home_assistant_token_ = token ? token : "";
  }

  void set_backlight(esphome::light::LightState *backlight) {
    backlight_ = backlight;
  }

  void set_screen_timeout(esphome::number::Number *timeout) {
    timeout_ = timeout;
  }

  void set_folder_select(HATiFolderSelect *folder_select) {
    folder_select_ = folder_select;
  }

  void setup() override;
  void loop() override;

 protected:
  std::string home_assistant_url_;
  std::string home_assistant_token_;
  esphome::light::LightState *backlight_ = nullptr;
  esphome::number::Number *timeout_ = nullptr;
  HATiFolderSelect *folder_select_ = nullptr;
  uint32_t renderer_start_after_ = 0;
};

}  // namespace web_admin_local
