// Weather tile: shows condition + temperature.
#include "../tiles_lvgl.h"
#include "../mdi_icons.h"
#include <lvgl.h>

namespace web_admin_local {

void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();

  const std::string &entity = tile.weather_entity.empty()
                              ? tile.sensor_entity : tile.weather_entity;
  const char *location = tile.title.empty()
      ? (entity.empty() ? "--" : entity.c_str()) : tile.title.c_str();

  lv_obj_t *t = lv_label_create(parent);
  lv_label_set_text(t, location);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_width(t, LV_PCT(70));
  lv_obj_set_style_text_color(t, white, 0);
  lv_obj_set_style_text_font(t, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(t, LV_ALIGN_TOP_RIGHT, 0, 4);

  // MDI weather icon, matching HomeTiles' top-left placement.
  lv_obj_t *icon = lv_label_create(parent);
  std::string icon_name = isMdiIconDisabled(tile.icon_name)
      ? std::string() : normalizeMdiIconName(tile.icon_name);
  const std::string icon_char = getMdiChar(
      icon_name.empty() ? "weather-partly-cloudy" : icon_name);
  lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xD5, 0x4F), 0);
  lv_obj_set_style_text_font(icon, ui_font_for_size(28), 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(40), 0);
  lv_obj_align(temp, LV_ALIGN_CENTER, 0, 8);

  lv_obj_t *condition = lv_label_create(parent);
  lv_label_set_text(condition, "--");
  lv_obj_set_style_text_color(condition, white, 0);
  lv_obj_set_style_text_font(condition, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(condition, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(condition, LV_PCT(100));
  lv_obj_align(condition, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_t *forecast[4] = {};
  uint8_t forecast_count = (tile.span_h >= 2 || tile.span_w >= 2) ? 4 : 0;
  for (uint8_t i = 0; i < forecast_count; ++i) {
    forecast[i] = lv_label_create(parent);
    lv_label_set_text(forecast[i], "--");
    lv_obj_set_style_text_color(forecast[i], white, 0);
    lv_obj_set_style_text_font(forecast[i], ui_font_for_size(12), 0);
    lv_obj_align(forecast[i], LV_ALIGN_BOTTOM_LEFT, 0, -static_cast<int>(i * 16));
  }
  if (!entity.empty()) {
    register_ha_weather_widget(entity, icon, temp, condition, forecast, forecast_count);
  }
}

}  // namespace web_admin_local
