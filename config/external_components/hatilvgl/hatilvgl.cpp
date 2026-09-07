#include "hatilvgl.h"

#include <atomic>

namespace web_admin_local {

namespace {
std::atomic<bool> g_api_connected{false};
}

void hatilvgl_set_api_connected(bool connected) {
  g_api_connected.store(connected, std::memory_order_release);
}

bool hatilvgl_is_api_connected() {
  return g_api_connected.load(std::memory_order_acquire);
}

}  // namespace web_admin_local
