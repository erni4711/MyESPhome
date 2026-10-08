#pragma once

namespace esphome::mipi_rgb {

void set_display_update_blocked(bool blocked);
bool is_display_update_blocked();

}  // namespace esphome::mipi_rgb
