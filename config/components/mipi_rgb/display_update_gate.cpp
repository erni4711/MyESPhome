#include "esphome/core/log.h"
#include "display_update_gate.h"

#include <atomic>

static const char* TAG = "energy_data";  

#if 1
#undef ESP_LOGE
#undef ESP_LOGD
#undef ESP_LOGI
#undef ESP_LOGW

#define ESP_LOGE(t, m, ...) printf("[%s:%d] " m "\n", t, __LINE__, ##__VA_ARGS__)
#define ESP_LOGD(t, m, ...) printf("[%s:%d] " m "\n", t, __LINE__, ##__VA_ARGS__)
#define ESP_LOGI(t, m, ...) printf("[%s:%d] " m "\n", t, __LINE__, ##__VA_ARGS__)
#define ESP_LOGW(t, m, ...) printf("[%s:%d] " m "\n", t, __LINE__, ##__VA_ARGS__)
#endif

const char* DISPLAY_UPDATE_GATE_TAG = "display_update_gate";
namespace esphome::mipi_rgb {

namespace {
std::atomic<bool> display_update_blocked{false};
}

void set_display_update_blocked(bool blocked) {
  if (blocked == display_update_blocked.load(std::memory_order_acquire)) {
    return;
  }
  display_update_blocked.store(blocked, std::memory_order_release);
  ESP_LOGD(DISPLAY_UPDATE_GATE_TAG, "Display update blocked: %s", blocked ? "true" : "false");  
}

bool is_display_update_blocked() {
  return display_update_blocked.load(std::memory_order_acquire);
}

}  // namespace esphome::mipi_rgb
