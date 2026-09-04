#pragma once

#include <lvgl.h>

LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_16);
LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_20_semibold);
LV_FONT_DECLARE(ui_font_24);
LV_FONT_DECLARE(ui_font_28);
LV_FONT_DECLARE(ui_font_40);
LV_FONT_DECLARE(ui_font_48);

namespace web_admin_local {

inline const lv_font_t *web_admin_lvgl_font_for_size(int size) {
  switch (size) {
    case 14: return &ui_font_14;
    case 16: return &ui_font_16;
    case 20: return &ui_font_20;
    case 24: return &ui_font_24;
    case 28: return &ui_font_28;
    case 40: return &ui_font_40;
    case 48: return &ui_font_48;
    default: return &ui_font_20;
  }
}

inline const lv_font_t *ui_font_for_size(uint8_t size) {
  return web_admin_lvgl_font_for_size(static_cast<int>(size));
}

inline const lv_font_t *web_admin_lvgl_font_semibold() {
  return &ui_font_20_semibold;
}

}  // namespace web_admin_local
