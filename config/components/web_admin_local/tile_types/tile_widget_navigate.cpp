// Navigate tile: tapping navigates to another folder page.
#include "../tiles_lvgl.h"
#include "../web_admin_lvgl_fonts.h"
#include <lvgl.h>

namespace web_admin_local {

void tile_widget_build_navigate(lv_obj_t *parent, const TileData &tile, int /*folder_id*/) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Arrow icon at centre-top
  lv_obj_t *arrow = lv_label_create(parent);
  lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(arrow, lv_color_make(0x26, 0xA6, 0x9A), 0);
  lv_obj_set_style_text_font(arrow, ui_font_for_size(14), 0);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, 0, 0);

  // Folder name
  const char *name = tile.title.empty() ? "Folder" : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, ui_font_for_size(14), 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

  // "Tap to enter" hint
  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "Tap to open");
  lv_obj_set_style_text_color(hint, muted, 0);
  lv_obj_set_style_text_font(hint, ui_font_for_size(14), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

}  // namespace web_admin_local
