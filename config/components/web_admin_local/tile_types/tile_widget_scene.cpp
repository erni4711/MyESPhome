// Scene / Script tile: tap-to-trigger action tile.
#include "../tiles_lvgl.h"
#include "../web_admin_lvgl_fonts.h"
#include <lvgl.h>

namespace web_admin_local {

void tile_widget_build_scene(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);

  // Large play icon
  lv_obj_t *icon = lv_label_create(parent);
  lv_label_set_text(icon, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(icon, accent, 0);
  lv_obj_set_style_text_font(icon, ui_font_for_size(28), 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);

  // Scene / script name
  const char *name = tile.title.empty() ? tile.scene_alias.c_str() : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
}

}  // namespace web_admin_local
