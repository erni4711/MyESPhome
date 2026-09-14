// Weather tile: HomeTiles-style condition, temperature, and forecast card.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <lvgl.h>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <ctime>

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

constexpr uint8_t kMaxForecastDays = 8;
constexpr const char *kCelsius = "\xC2\xB0" "C";

lv_obj_t *weather_popup_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text ? text : "--");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

void style_weather_popup_section(lv_obj_t *section) {
  lv_obj_set_style_bg_opa(section, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(section, lv_color_make(0x3A, 0x4A, 0x5A), 0);
  lv_obj_set_style_border_width(section, 1, 0);
  lv_obj_set_style_border_side(section,
                               static_cast<lv_border_side_t>(
                                   LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
                               0);
  lv_obj_set_style_radius(section, 0, 0);
  lv_obj_set_style_pad_all(section, 4, 0);
  lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);
}

lv_color_t weather_temperature_color(float temperature) {
  if (temperature < 0.0f)
    return lv_color_make(0x87, 0xCE, 0xEB);
  if (temperature < 18.0f) {
    return lv_color_make(0x4D, 0xB3, 0xFF);
  }
  if (temperature < 25.0f) return lv_color_white();
  if (temperature < 30.0f) {
    const float ratio = (temperature - 25.0f) / 5.0f;
    return lv_color_make(0xFF, static_cast<uint8_t>(0xE0 - ratio * 0x70),
                         static_cast<uint8_t>(0x30 - ratio * 0x30));
  }
  if (temperature < 40.0f) {
    const float ratio = (temperature - 30.0f) / 10.0f;
    return lv_color_make(static_cast<uint8_t>(0xF4 - ratio * (0xF4 - 0x8B)),
                         static_cast<uint8_t>(0x43 - ratio * 0x43),
                         static_cast<uint8_t>(0x36 - ratio * 0x36));
  }
  return lv_color_make(0x8B, 0x00, 0x00);
}

float weather_temperature_value(const char *text) {
  char *end = nullptr;
  const float value = strtof(text ? text : "", &end);
  return end != (text ? text : "") ? value : 0.0f;
}

int weather_rain_bar_height(float precipitation) {
  if (precipitation <= 0.0f) return 0;
  return std::min(42, std::max(4, static_cast<int>(std::lround(precipitation * 4.0f))));
}

void format_weather_temperature(char *buffer, size_t length,
                                const char *text) {
  snprintf(buffer, length, "%.1f %s",
           static_cast<double>(weather_temperature_value(text)), kCelsius);
}

lv_obj_t *weather_forecast_column(lv_obj_t *parent, lv_obj_t *source = nullptr,
                                  bool hide_temperatures = false) {
  lv_obj_t *column = lv_obj_create(parent);
  lv_obj_set_width(column, LV_PCT(100));
  lv_obj_set_height(column, source ? 180 : 120);
  lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(column, 0, 0);
  lv_obj_set_style_pad_all(column, 0, 0);
  lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);

  const char *day = source ? lv_label_get_text(lv_obj_get_child(source, 0)) : "--";
  const char *icon = source ? lv_label_get_text(lv_obj_get_child(source, 1)) : "?";
  const char *high = source ? lv_label_get_text(lv_obj_get_child(source, 2)) : "--";
  const char *low = source ? lv_label_get_text(lv_obj_get_child(source, 3)) : "--";
  lv_obj_t *day_label = weather_popup_label(column, day, ui_font_for_size(16));
  lv_obj_t *icon_label = weather_popup_label(column, icon, FONT_MDI_ICONS);
  lv_obj_set_style_text_color(icon_label, lv_color_white(), 0);
  lv_obj_t *high_label = weather_popup_label(column, high, ui_font_for_size(12));
  lv_obj_t *low_label = weather_popup_label(column, low, ui_font_for_size(12));
  lv_obj_set_style_text_color(
      high_label, weather_temperature_color(weather_temperature_value(high)),
      LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(
      low_label, weather_temperature_color(weather_temperature_value(low)),
      LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_t *amount = weather_popup_label(
      column, source ? lv_label_get_text(lv_obj_get_child(source, 4)) : "0.0 mm",
      ui_font_for_size(10));
  lv_obj_t *chance = weather_popup_label(
      column, source ? lv_label_get_text(lv_obj_get_child(source, 5)) : "0 %",
      ui_font_for_size(10));
  lv_obj_add_flag(amount, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(chance, LV_OBJ_FLAG_HIDDEN);
  if (hide_temperatures) {
    lv_obj_add_flag(high_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(low_label, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_width(day_label, LV_PCT(100));
  lv_obj_set_width(icon_label, LV_PCT(100));
  lv_obj_set_width(high_label, LV_PCT(100));
  lv_obj_set_width(low_label, LV_PCT(100));
  lv_label_set_long_mode(day_label, LV_LABEL_LONG_CLIP);
  lv_label_set_long_mode(icon_label, LV_LABEL_LONG_CLIP);
  lv_obj_align(day_label, LV_ALIGN_TOP_MID, 0, 0);
  // Normal weather tiles use these columns directly. Keep the icon in its
  // own band so its glyph cannot overlap the high-temperature label.
  lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, source ? 26 : 14);
  lv_obj_align(high_label, LV_ALIGN_TOP_MID, 0, source ? 54 : 66);
  lv_obj_align(low_label, LV_ALIGN_TOP_MID, 0, source ? 73 : 88);
  return column;
}

lv_obj_t *add_temperature_labels(lv_obj_t *parent, lv_obj_t **forecast,
                                  uint8_t forecast_count, bool high,
                                  lv_obj_t **output) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 20);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
  for (uint8_t i = 0; i < forecast_count; ++i) {
    const int child_index = high ? 2 : 3;
    const char *text =
        lv_label_get_text(lv_obj_get_child(forecast[i], child_index));
    char temperature_text[16];
    format_weather_temperature(temperature_text, sizeof(temperature_text), text);
    lv_obj_t *label =
        weather_popup_label(row, temperature_text, ui_font_for_size(11));
    if (output) output[i] = label;
    lv_obj_set_style_text_color(
        label, weather_temperature_color(weather_temperature_value(text)),
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(label, LV_PCT(100 / forecast_count));
  }
  return row;
}

void close_weather_popup(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  lv_obj_t *button =
      static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_obj_t *panel = lv_obj_get_parent(button);
  lv_obj_del(lv_obj_get_parent(panel));
}

void draw_hourly_temperature_chart(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
  lv_obj_t *chart = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_layer_t *layer = lv_event_get_layer(event);
  lv_area_t area;
  lv_obj_get_coords(chart, &area);
  const int width = lv_area_get_width(&area);
  const int height = lv_area_get_height(&area);
  float minimum = 1000.0f;
  float maximum = -1000.0f;
  long first_timestamp = 0;
  int valid_count = 0;
  for (int i = 0; i < 48; ++i) {
    if (!hourly_weather_valid[i]) continue;
    minimum = std::min(minimum, hourly_weather_temperature[i]);
    maximum = std::max(maximum, hourly_weather_temperature[i]);
    if (first_timestamp == 0 || hourly_weather_timestamp[i] < first_timestamp)
      first_timestamp = hourly_weather_timestamp[i];
    ++valid_count;
  }
  if (valid_count < 2) return;
  // Align the hourly line with the daily columns: the first point is placed
  // at its time within day one and the 47-hour forecast ends in day three.
  time_t first_time = static_cast<time_t>(first_timestamp);
  struct tm first_day;
  localtime_r(&first_time, &first_day);
  first_day.tm_hour = 0;
  first_day.tm_min = 0;
  first_day.tm_sec = 0;
  const long chart_start = static_cast<long>(mktime(&first_day));
  constexpr long kDailyColumnCount = 8;
  constexpr long kSecondsPerDay = 24 * 60 * 60;
  constexpr long kSecondsPerHour = 60 * 60;
  const long chart_end = chart_start + kDailyColumnCount * kSecondsPerDay;
  const long first_time_offset = std::max(
      0L, std::min(kSecondsPerDay - 1, first_timestamp - chart_start));
  if (maximum - minimum < 1.0f) {
    minimum -= 0.5f;
    maximum += 0.5f;
  }

  lv_point_precise_t points[48];
  int point_count = 0;
  int forecast_index = 0;
  for (int i = 0; i < 48; ++i) {
    if (!hourly_weather_valid[i]) continue;
    const float ratio =
        (hourly_weather_temperature[i] - minimum) / (maximum - minimum);
    const float time_ratio = std::max(
        0.0f, std::min(1.0f,
                       static_cast<float>(first_time_offset +
                                          forecast_index * kSecondsPerHour) /
                           static_cast<float>(chart_end - chart_start)));
    points[point_count].x =
        area.x1 + static_cast<int>(time_ratio * width);
    points[point_count].y = area.y2 - 4 -
                             static_cast<int>(ratio * (height - 8));
    ++point_count;
    ++forecast_index;
  }
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = lv_color_make(0xFF, 0xB7, 0x3B);
  line.width = 3;
  line.round_start = 1;
  line.round_end = 1;
  line.points = points;
  line.point_cnt = point_count;
  lv_draw_line(layer, &line);
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
  lv_obj_set_size(panel, LV_PCT(92), 500);
  lv_obj_set_style_bg_color(panel, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(panel, 18, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 16, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *header_icon = weather_popup_label(
      panel, lv_label_get_text(context->icon), FONT_MDI_ICONS);
  lv_obj_set_style_text_color(header_icon, lv_color_white(), 0);
  lv_obj_align(header_icon, LV_ALIGN_TOP_LEFT, 18, 12);
  lv_obj_t *title = weather_popup_label(panel, context->title.c_str(),
                                        ui_font_for_size(16));
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 55, 15);

  lv_obj_t *close = lv_button_create(panel);
  lv_obj_set_size(close, 42, 42);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(close, 0, 0);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -8, 7);
  lv_obj_t *close_label = lv_label_create(close);
  lv_label_set_text(close_label, getMdiChar("close").c_str());
  lv_obj_set_style_text_font(close_label, FONT_MDI_ICONS, 0);
  lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
  lv_obj_center(close_label);
  lv_obj_add_event_cb(close, close_weather_popup, LV_EVENT_CLICKED, nullptr);

  std::string headline = lv_label_get_text(context->condition);
  headline += "  |  ";
  char current_temperature[16];
  format_weather_temperature(current_temperature, sizeof(current_temperature),
                             lv_label_get_text(context->temperature));
  headline += current_temperature;
  lv_obj_t *summary = weather_popup_label(panel, headline.c_str(),
                                          ui_font_for_size(22));
  lv_obj_set_width(summary, LV_PCT(100));
  lv_obj_set_style_text_color(summary, lv_color_make(0xD8, 0xF0, 0xFF), 0);
  lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 48);

  lv_obj_t *forecast_row = lv_obj_create(panel);
  lv_obj_set_size(forecast_row, LV_PCT(100), 100);
  style_weather_popup_section(forecast_row);
  lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_align(forecast_row, LV_ALIGN_TOP_MID, 0, 86);

  for (uint8_t i = 0; i < context->forecast_count; ++i) {
    lv_obj_t *column = weather_forecast_column(
        forecast_row, context->forecast[i]);
    lv_obj_set_width(column, LV_PCT(100 / context->forecast_count));
    lv_obj_set_height(column, 90);
    lv_obj_add_flag(lv_obj_get_child(column, 2), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(column, 3), LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_t *high_row = add_temperature_labels(
      panel, context->forecast, context->forecast_count, true, nullptr);
  lv_obj_align(high_row, LV_ALIGN_TOP_MID, 0, 192);

  lv_obj_t *temperature_row = lv_obj_create(panel);
  lv_obj_set_size(temperature_row, LV_PCT(100), 100);
  lv_obj_set_style_bg_opa(temperature_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(temperature_row, 0, 0);
  lv_obj_set_style_pad_all(temperature_row, 4, 0);
  lv_obj_set_flex_flow(temperature_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(temperature_row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(temperature_row, LV_ALIGN_TOP_MID, 0, 215);
  lv_obj_add_event_cb(temperature_row, draw_hourly_temperature_chart,
                      LV_EVENT_DRAW_MAIN, nullptr);

  lv_obj_t *low_row = add_temperature_labels(
      panel, context->forecast, context->forecast_count, false, nullptr);
  lv_obj_align(low_row, LV_ALIGN_TOP_MID, 0, 322);

  lv_obj_t *rain_row = lv_obj_create(panel);
  lv_obj_set_size(rain_row, LV_PCT(100), 120);
  style_weather_popup_section(rain_row);
  lv_obj_set_flex_flow(rain_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rain_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(rain_row, LV_ALIGN_TOP_MID, 0, 350);
  for (uint8_t i = 0; i < context->forecast_count; ++i) {
    lv_obj_t *rain_column = lv_obj_create(rain_row);
    lv_obj_set_width(rain_column, LV_PCT(100 / context->forecast_count));
    lv_obj_set_height(rain_column, 100);
    lv_obj_set_style_bg_opa(rain_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rain_column, 0, 0);
    lv_obj_set_style_pad_all(rain_column, 0, 0);
    lv_obj_set_layout(rain_column, LV_LAYOUT_NONE);
    const float amount = static_cast<float>(atof(
        lv_label_get_text(lv_obj_get_child(context->forecast[i], 4))));
    const float probability = static_cast<float>(atof(
        lv_label_get_text(lv_obj_get_child(context->forecast[i], 5))));
    lv_obj_t *track = lv_obj_create(rain_column);
    lv_obj_set_width(track, 14);
    lv_obj_set_height(track, 34);
    lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_bg_color(track, lv_color_make(0x18, 0x3B, 0x52), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = lv_obj_create(track);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, weather_rain_bar_height(amount));
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, lv_color_make(0x29, 0x96, 0xF3), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    char probability_text[12];
    snprintf(probability_text, sizeof(probability_text), "%.0f %%",
             probability);
    lv_obj_t *amount_label = weather_popup_label(
        rain_column, lv_label_get_text(lv_obj_get_child(context->forecast[i], 4)),
        ui_font_for_size(10));
    lv_obj_set_width(amount_label, LV_PCT(100));
    lv_obj_align(amount_label, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t *probability_label =
        weather_popup_label(rain_column, probability_text, ui_font_for_size(10));
    lv_obj_set_width(probability_label, LV_PCT(100));
    lv_obj_align(probability_label, LV_ALIGN_TOP_MID, 0, 55);
  }

}

}  // namespace

void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  // The inline chart/forecast layout needs more than two grid rows. Wide
  // short tiles use the regular compact forecast columns instead.
  const bool inline_weather = tile.span_w >= 7 && tile.span_h > 2;
  const int inline_height = lv_obj_get_height(parent);
  const bool compact_inline = inline_weather &&
                              ((inline_height > 0 && inline_height < 380) ||
                               tile.span_h <= 2);
  const int inline_chart_y = compact_inline ? 270 : 275;
  const int inline_rain_y = compact_inline ? 352 : 410;

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
  lv_obj_set_style_text_color(icon, white, 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, -2);

  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(22), 0);
  lv_obj_set_style_text_align(temp, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(temp, LV_PCT(100));
  lv_obj_align(temp, LV_ALIGN_TOP_MID, 0, 62);

  lv_obj_t *condition = lv_label_create(parent);
  lv_label_set_text(condition, "--");
  lv_obj_set_style_text_color(condition, white, 0);
  lv_obj_set_style_text_font(condition, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(condition, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(condition, LV_PCT(100));
  lv_obj_align(condition, LV_ALIGN_TOP_MID, 0, 38);

  lv_obj_t *forecast[kMaxForecastDays] = {};
  lv_obj_t *high_labels[kMaxForecastDays] = {};
  lv_obj_t *low_labels[kMaxForecastDays] = {};
  const uint8_t display_forecast_count = inline_weather || tile.span_h >= 2
      ? static_cast<uint8_t>(compact_inline
                                 ? 4
                                 : (tile.span_w >= 7
                                      ? kMaxForecastDays
                                      : (tile.span_w >= 5 ? 7 : 4)))
      : 0;
  constexpr uint8_t popup_forecast_count = kMaxForecastDays;
  lv_obj_t *forecast_row = nullptr;
  if (display_forecast_count > 0 || !inline_weather) {
    forecast_row = lv_obj_create(parent);
    lv_obj_set_size(forecast_row, LV_PCT(100),
                    inline_weather ? (compact_inline ? 120 : 145) : 120);
    if (inline_weather) {
      style_weather_popup_section(forecast_row);
      lv_obj_set_style_pad_all(forecast_row, 0, 0);
      lv_obj_set_style_border_width(forecast_row, 1, 0);
      lv_obj_set_style_border_side(
          forecast_row, static_cast<lv_border_side_t>(
                            LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    } else {
      lv_obj_set_style_bg_opa(forecast_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(forecast_row, 0, 0);
      lv_obj_set_style_pad_all(forecast_row, 0, 0);
    }
    lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (inline_weather) {
      lv_obj_align(forecast_row, LV_ALIGN_TOP_MID, 0,
                   compact_inline ? 82 : 105);
    } else {
      lv_obj_align(forecast_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    lv_obj_clear_flag(forecast_row, LV_OBJ_FLAG_SCROLLABLE);
    if (display_forecast_count == 0)
      lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);
  }
  for (uint8_t i = 0; i < popup_forecast_count; ++i) {
    if (forecast_row == nullptr) break;
    forecast[i] = weather_forecast_column(forecast_row, nullptr,
                                          inline_weather);
    const uint8_t column_count = inline_weather
        ? display_forecast_count : popup_forecast_count;
    lv_obj_set_width(forecast[i], LV_PCT(100 / column_count));
    if (compact_inline) {
      lv_obj_set_style_text_font(lv_obj_get_child(forecast[i], 0),
                                 ui_font_for_size(14), 0);
      lv_obj_set_style_text_font(lv_obj_get_child(forecast[i], 1),
                                 ui_font_for_size(24), 0);
      lv_obj_align(lv_obj_get_child(forecast[i], 1), LV_ALIGN_TOP_MID, 0, -2);
    }
    if (i >= display_forecast_count)
      lv_obj_add_flag(forecast[i], LV_OBJ_FLAG_HIDDEN);
  }

  if (inline_weather) {
    lv_obj_align(add_temperature_labels(parent, forecast, display_forecast_count, true,
                                        high_labels),
                 LV_ALIGN_TOP_MID, 0, compact_inline ? 246 : 250);
    lv_obj_t *temperature_row = lv_obj_create(parent);
    lv_obj_set_size(temperature_row, LV_PCT(94), 60);
    lv_obj_set_style_bg_opa(temperature_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(temperature_row, 0, 0);
    lv_obj_set_style_pad_all(temperature_row, 0, 0);
    lv_obj_align(temperature_row, LV_ALIGN_TOP_MID, 0, inline_chart_y + 20);
    lv_obj_clear_flag(temperature_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(temperature_row, draw_hourly_temperature_chart,
                        LV_EVENT_DRAW_MAIN, nullptr);
    if (compact_inline) lv_obj_add_flag(temperature_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *precipitation_labels[kMaxForecastDays] = {};
    lv_obj_t *probability_labels[kMaxForecastDays] = {};
    lv_obj_t *precipitation_bars[kMaxForecastDays] = {};
    lv_obj_t *rain_row = lv_obj_create(parent);
    lv_obj_set_size(rain_row, LV_PCT(94), 90);
    style_weather_popup_section(rain_row);
    lv_obj_set_style_pad_all(rain_row, 0, 0);
    lv_obj_set_style_border_width(rain_row, 1, 0);
    lv_obj_set_style_border_side(
        rain_row, static_cast<lv_border_side_t>(
                     LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_flex_flow(rain_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rain_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(rain_row, LV_ALIGN_TOP_MID, 0, inline_rain_y + 12);
    lv_obj_clear_flag(rain_row, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < display_forecast_count; ++i) {
      lv_obj_t *rain_column = lv_obj_create(rain_row);
      lv_obj_set_width(rain_column, LV_PCT(100 / display_forecast_count));
      lv_obj_set_height(rain_column, 90);
      lv_obj_set_style_bg_opa(rain_column, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(rain_column, 0, 0);
      lv_obj_set_style_pad_all(rain_column, 0, 0);
      lv_obj_clear_flag(rain_column, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scrollbar_mode(rain_column, LV_SCROLLBAR_MODE_OFF);
      lv_obj_t *track = lv_obj_create(rain_column);
      lv_obj_set_width(track, 14);
      lv_obj_set_height(track, 42);
      lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 0);
      lv_obj_set_style_radius(track, 4, 0);
      lv_obj_set_style_bg_color(track, lv_color_make(0x62, 0x89, 0x9A), 0);
      lv_obj_set_style_bg_opa(track, LV_OPA_70, 0);
      lv_obj_set_style_border_width(track, 1, 0);
      lv_obj_set_style_border_color(track, lv_color_make(0xA7, 0xD8, 0xF2), 0);
      lv_obj_set_style_pad_all(track, 0, 0);
      lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
      const float amount = static_cast<float>(
          atof(lv_label_get_text(lv_obj_get_child(forecast[i], 4))));
      lv_obj_t *bar = lv_obj_create(track);
      lv_obj_set_width(bar, LV_PCT(100));
      lv_obj_set_height(bar, weather_rain_bar_height(amount));
      lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
      lv_obj_set_style_radius(bar, 3, 0);
      lv_obj_set_style_bg_color(bar, lv_color_make(0x29, 0x96, 0xF3), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(bar, 0, 0);
      lv_obj_set_style_pad_all(bar, 0, 0);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
      precipitation_bars[i] = bar;
      const float probability = static_cast<float>(
          atof(lv_label_get_text(lv_obj_get_child(forecast[i], 5))));
      char probability_text[12];
      snprintf(probability_text, sizeof(probability_text), "%.0f %%",
               probability);
      precipitation_labels[i] = weather_popup_label(
          rain_column, lv_label_get_text(lv_obj_get_child(forecast[i], 4)),
          ui_font_for_size(10));
      lv_obj_align(precipitation_labels[i], LV_ALIGN_TOP_MID, 0, 46);
      probability_labels[i] = weather_popup_label(
          rain_column, probability_text, ui_font_for_size(10));
      lv_obj_align(probability_labels[i], LV_ALIGN_TOP_MID, 0, 67);
    }
    lv_obj_align(add_temperature_labels(parent, forecast, display_forecast_count, false,
                                        low_labels),
                 LV_ALIGN_TOP_MID, 0,
                 compact_inline ? 272 : 380);
    // The chart and its labels occupy a fixed vertical band. Keep rainfall
    // below the low-temperature row so the probability text is not clipped.
    lv_obj_align(rain_row, LV_ALIGN_TOP_MID, 0, inline_rain_y);
    if (compact_inline) lv_obj_add_flag(rain_row, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < display_forecast_count; ++i) {
      lv_obj_set_style_text_align(precipitation_labels[i], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_align(probability_labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!entity.empty()) {
      register_ha_weather_widget(entity, icon, temp, condition, forecast,
                                 popup_forecast_count, high_labels, low_labels,
                                 precipitation_labels, probability_labels,
                                 precipitation_bars);
    }
  }

  if (inline_weather) {
    return;
  }

  auto *popup_context = new WeatherPopupContext{};
  popup_context->icon = icon;
  popup_context->temperature = temp;
  popup_context->condition = condition;
  popup_context->forecast_count = popup_forecast_count;
  popup_context->forecast = new lv_obj_t *[popup_forecast_count];
  for (uint8_t i = 0; i < popup_forecast_count; ++i) {
    popup_context->forecast[i] = forecast[i];
  }
  popup_context->title = location;
  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, weather_click_cb, LV_EVENT_CLICKED,
                      popup_context);
  lv_obj_add_event_cb(parent, weather_popup_delete_cb, LV_EVENT_DELETE,
                      popup_context);

  if (!entity.empty()) {
    register_ha_weather_widget(entity, icon, temp, condition, forecast,
                               popup_forecast_count);
  }
}

}  // namespace web_admin_local
