#include "hatilvgl.h"

#include <atomic>

namespace web_admin_local {

namespace {
std::atomic<bool> g_api_connected{false};
esphome::light::LightState *g_backlight = nullptr;
esphome::number::Number *g_timeout = nullptr;
}

void hatilvgl_set_api_connected(bool connected) {
  g_api_connected.store(connected, std::memory_order_release);
}

bool hatilvgl_is_api_connected() {
  return g_api_connected.load(std::memory_order_acquire);
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

}  // namespace web_admin_local
