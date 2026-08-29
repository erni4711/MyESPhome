// Weather tile: shows condition + temperature.
#include "../tiles_lvgl.h"
#include "../web_admin_lvgl_fonts.h"
#include <lvgl.h>

namespace web_admin_local {

void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Title
  if (!tile.title.empty()) {
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, tile.title.c_str());
    lv_obj_set_style_text_color(t, muted, 0);
    lv_obj_set_style_text_font(t, ui_font_for_size(14), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Weather icon placeholder (sun symbol)
  lv_obj_t *icon = lv_label_create(parent);
  lv_label_set_text(icon, LV_SYMBOL_WARNING);  // placeholder until icon map implemented
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xD5, 0x4F), 0);
  lv_obj_set_style_text_font(icon, ui_font_for_size(14), 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, -20, 0);

  // Temperature placeholder
  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--°");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(14), 0);
  lv_obj_align(temp, LV_ALIGN_CENTER, 28, 0);

  // Entity hint
  const std::string &entity = tile.weather_entity.empty()
                              ? tile.sensor_entity : tile.weather_entity;
  if (!entity.empty()) {
    lv_obj_t *e = lv_label_create(parent);
    lv_label_set_text(e, entity.c_str());
    lv_label_set_long_mode(e, LV_LABEL_LONG_DOT);
    lv_obj_set_width(e, LV_PCT(100));
    lv_obj_set_style_text_color(e, muted, 0);
    lv_obj_set_style_text_font(e, ui_font_for_size(14), 0);
    lv_obj_align(e, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
}

}  // namespace web_admin_local
