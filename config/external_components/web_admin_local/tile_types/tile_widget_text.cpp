// Text tile: displays a static text value.
#include "../tiles_lvgl.h"
#include <lvgl.h>

namespace web_admin_local {

void tile_widget_build_text(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Optional small title at top
  if (!tile.title.empty()) {
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, tile.title.c_str());
    lv_obj_set_style_text_color(t, muted, 0);
    lv_obj_set_style_text_font(t, ui_font_for_size(14), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Choose font based on text_value_font index (0=default 28, 1=36, etc.)
  const lv_font_t *font;
  switch (tile.text_value_font) {
    case 1:  font = ui_font_for_size(28); break;
    case 2:  font = ui_font_for_size(28); break;
    default: font = ui_font_for_size(20); break;
  }

  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, tile.text_value.empty() ? "--" : tile.text_value.c_str());
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, tile.title.empty() ? 0 : 8);
}

}  // namespace web_admin_local
