// Weather tile: HomeTiles-style condition, temperature, and forecast card.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <lvgl.h>

namespace web_admin_local {

namespace {

struct WeatherPopupContext {
  lv_obj_t *icon = nullptr;
  lv_obj_t *temperature = nullptr;
  lv_obj_t *condition = nullptr;
  lv_obj_t **forecast = nullptr;
  uint8_t forecast_count = 0;
  std::string title;
};

lv_obj_t *weather_popup_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, lv_coord_t y) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text ? text : "--");
  lv_obj_set_width(label, LV_PCT(100));
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  return label;
}

void close_weather_popup(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  lv_obj_t *button =
      static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_obj_t *panel = lv_obj_get_parent(button);
  lv_obj_del(lv_obj_get_parent(panel));
}

void weather_popup_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
  auto *context =
      static_cast<WeatherPopupContext *>(lv_event_get_user_data(event));
  delete[] context->forecast;
  delete context;
}

void weather_click_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<WeatherPopupContext *>(lv_event_get_user_data(event));
  if (!context) return;

  lv_obj_t *overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *panel = lv_obj_create(overlay);
  lv_obj_set_size(panel, LV_PCT(86), 300);
  lv_obj_set_style_bg_color(panel, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

  weather_popup_label(panel, context->title.c_str(), ui_font_for_size(18), 10);
  lv_obj_t *icon = weather_popup_label(
      panel, lv_label_get_text(context->icon), FONT_MDI_ICONS, 42);
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xD5, 0x4F), 0);
  weather_popup_label(panel, lv_label_get_text(context->temperature),
                      ui_font_for_size(30), 82);
  weather_popup_label(panel, lv_label_get_text(context->condition),
                      ui_font_for_size(14), 126);

  for (uint8_t i = 0; i < context->forecast_count; ++i) {
    lv_obj_t *forecast = weather_popup_label(
        panel, lv_label_get_text(context->forecast[i]), ui_font_for_size(12),
        static_cast<lv_coord_t>(158 + i * 22));
    lv_obj_set_style_text_align(forecast, LV_TEXT_ALIGN_LEFT, 0);
  }

  lv_obj_t *close = lv_button_create(panel);
  lv_obj_set_size(close, 80, 32);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_t *close_label = lv_label_create(close);
  lv_label_set_text(close_label, "Close");
  lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
  lv_obj_center(close_label);
  lv_obj_add_event_cb(close, close_weather_popup, LV_EVENT_CLICKED, nullptr);
}

}  // namespace

void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();

  const std::string &entity = tile.entity_id;
  const char *location = tile.title.empty()
      ? (entity.empty() ? "Weather" : entity.c_str()) : tile.title.c_str();

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
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, -2);

  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(32), 0);
  lv_obj_set_style_text_align(temp, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(temp, LV_PCT(100));
  lv_obj_align(temp, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *condition = lv_label_create(parent);
  lv_label_set_text(condition, "--");
  lv_obj_set_style_text_color(condition, white, 0);
  lv_obj_set_style_text_font(condition, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(condition, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(condition, LV_PCT(100));
  lv_obj_align(condition, LV_ALIGN_BOTTOM_MID, 0, -28);

  lv_obj_t *forecast[4] = {};
  const uint8_t forecast_count = tile.span_h >= 2 ? 4 : 0;
  lv_obj_t *forecast_row = nullptr;
  if (forecast_count > 0) {
    forecast_row = lv_obj_create(parent);
    lv_obj_set_size(forecast_row, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(forecast_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(forecast_row, 0, 0);
    lv_obj_set_style_pad_all(forecast_row, 0, 0);
    lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(forecast_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(forecast_row, LV_OBJ_FLAG_SCROLLABLE);
  }
  for (uint8_t i = 0; i < forecast_count; ++i) {
    forecast[i] = lv_label_create(forecast_row);
    lv_label_set_text(forecast[i], "--");
    lv_obj_set_style_text_color(forecast[i], white, 0);
    lv_obj_set_style_text_font(forecast[i], ui_font_for_size(12), 0);
    lv_obj_set_style_text_align(forecast[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(forecast[i], LV_PCT(24));
    lv_label_set_long_mode(forecast[i], LV_LABEL_LONG_DOT);
  }

  auto *popup_context = new WeatherPopupContext{};
  popup_context->icon = icon;
  popup_context->temperature = temp;
  popup_context->condition = condition;
  popup_context->forecast_count = forecast_count;
  popup_context->forecast = new lv_obj_t *[forecast_count];
  for (uint8_t i = 0; i < forecast_count; ++i) {
    popup_context->forecast[i] = forecast[i];
  }
  popup_context->title = location;
  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, weather_click_cb, LV_EVENT_CLICKED,
                      popup_context);
  lv_obj_add_event_cb(parent, weather_popup_delete_cb, LV_EVENT_DELETE,
                      popup_context);

  if (!entity.empty()) {
    register_ha_weather_widget(entity, icon, temp, condition, forecast, forecast_count);
  }
}

}  // namespace web_admin_local
