#include "energy_data.h"
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"

#include <lvgl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace web_admin_local {
namespace {

struct EnergyTileContext {
  std::string statistic_id;
  std::string title;
  std::string configured_unit;
  int decimals = 1;
};

struct EnergyPopupContext {
  std::string statistic_id;
  std::string title;
  std::string configured_unit;
  int decimals = 1;
  bool week = false;
  lv_obj_t *overlay = nullptr;
  lv_obj_t *value_label = nullptr;
  lv_obj_t *status_label = nullptr;
  lv_obj_t *day_button = nullptr;
  lv_obj_t *week_button = nullptr;
  lv_obj_t *chart = nullptr;
  lv_obj_t *readout = nullptr;
  lv_obj_t *zero_line = nullptr;
  std::array<lv_obj_t *, 24> bars{};
  std::array<lv_obj_t *, 24> axis_labels{};
  lv_timer_t *timer = nullptr;
  uint32_t rendered_generation = UINT32_MAX;
  bool rendered_week = false;
};

EnergyPopupContext *g_energy_popup = nullptr;

const lv_font_t *energy_value_font(int configured) {
  switch (configured) {
    case 1: return ui_font_for_size(20);
    case 2: return ui_font_for_size(24);
    case 3: return ui_font_for_size(32);
    case 4: return ui_font_for_size(40);
    default: return ui_font_for_size(28);
  }
}

std::string effective_unit(const EnergyPopupContext &context,
                           const EnergySnapshot &snapshot) {
  return context.configured_unit.empty() ? snapshot.unit
                                         : context.configured_unit;
}

std::string format_energy_value(float value, int decimals,
                                const std::string &unit) {
  char buffer[64];
  const int digits = std::max(0, std::min(6, decimals));
  std::snprintf(buffer, sizeof(buffer), "%.*f%s%s", digits, value,
                unit.empty() ? "" : " ", unit.c_str());
  return buffer;
}

void style_range_button(lv_obj_t *button, bool selected) {
  lv_obj_set_style_bg_color(
      button, selected ? lv_color_white() : lv_color_make(0x45, 0x45, 0x45), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_t *label = lv_obj_get_child(button, 0);
  if (label) {
    lv_obj_set_style_text_color(
        label, selected ? lv_color_make(0x2A, 0x2A, 0x2A) : lv_color_white(), 0);
  }
}

void clear_chart(EnergyPopupContext *context) {
  if (context->zero_line) lv_obj_del(context->zero_line);
  context->zero_line = nullptr;
  for (lv_obj_t *&bar : context->bars) {
    if (bar) lv_obj_del(bar);
    bar = nullptr;
  }
  for (lv_obj_t *&label : context->axis_labels) {
    if (label) lv_obj_del(label);
    label = nullptr;
  }
}

const char *weekday_text(int64_t start_ms, char *buffer, size_t size) {
  if (start_ms <= 0) {
    std::snprintf(buffer, size, "-");
    return buffer;
  }
  const time_t start = static_cast<time_t>(start_ms / 1000);
  struct tm local {};
  localtime_r(&start, &local);
  strftime(buffer, size, "%a", &local);
  return buffer;
}

void update_popup(EnergyPopupContext *context) {
  if (!context || !context->chart) return;
  EnergySnapshot snapshot;
  if (!energy_get_snapshot(context->statistic_id, snapshot)) {
    lv_label_set_text(context->value_label, "--");
    lv_label_set_text(context->status_label, "Loading Energy statistics...");
    return;
  }

  const std::string unit = effective_unit(*context, snapshot);
  if (snapshot.today_total_valid) {
    const std::string total =
        format_energy_value(snapshot.today_total, context->decimals, unit);
    lv_label_set_text(context->value_label, total.c_str());
  } else {
    lv_label_set_text(context->value_label,
                      snapshot.error ? "Unavailable" : "--");
  }
  if (!snapshot.configured) {
    lv_label_set_text(context->status_label,
                      "Statistic is not configured in the Energy Dashboard");
  } else if (snapshot.error) {
    lv_label_set_text(context->status_label, snapshot.error_message.c_str());
  } else if (snapshot.loading) {
    lv_label_set_text(context->status_label, "Loading...");
  } else {
    lv_label_set_text(context->status_label,
                      context->week ? "Last 7 days" : "Today by hour");
  }

  style_range_button(context->day_button, !context->week);
  style_range_button(context->week_button, context->week);
  if (context->rendered_generation == snapshot.generation &&
      context->rendered_week == context->week)
    return;
  context->rendered_generation = snapshot.generation;
  context->rendered_week = context->week;
  clear_chart(context);

  const int slots = context->week ? 7 : 24;
  float minimum = 0.0f;
  float maximum = 0.0f;
  bool any = false;
  for (int i = 0; i < slots; ++i) {
    const EnergyBucket &bucket =
        context->week ? snapshot.week[i] : snapshot.day[i];
    if (!bucket.valid) continue;
    minimum = std::min(minimum, bucket.value);
    maximum = std::max(maximum, bucket.value);
    any = true;
  }
  if (!any) {
    lv_label_set_text(context->status_label,
                      snapshot.loading ? "Loading..." : "No statistics data");
    return;
  }
  if (maximum - minimum < 0.001f) {
    maximum += 0.5f;
    minimum -= 0.5f;
  }

  constexpr int chart_width = 690;
  constexpr int chart_height = 300;
  constexpr int axis_height = 28;
  const int gap = context->week ? 12 : 4;
  const int bar_width =
      std::max(4, (chart_width - gap * (slots + 1)) / slots);
  const float range = maximum - minimum;
  const int zero_y = static_cast<int>(
      std::lround(chart_height * maximum / range));

  context->zero_line = lv_obj_create(context->chart);
  lv_obj_set_size(context->zero_line, chart_width, 1);
  lv_obj_set_pos(context->zero_line, 0, zero_y);
  lv_obj_set_style_bg_color(
      context->zero_line, lv_color_make(0x80, 0x80, 0x80), 0);
  lv_obj_set_style_bg_opa(context->zero_line, LV_OPA_60, 0);
  lv_obj_set_style_border_width(context->zero_line, 0, 0);

  for (int i = 0; i < slots; ++i) {
    const EnergyBucket &bucket =
        context->week ? snapshot.week[i] : snapshot.day[i];
    const int x = gap + i * (bar_width + gap);
    if (bucket.valid) {
      const int value_y = static_cast<int>(
          std::lround(chart_height * (maximum - bucket.value) / range));
      const int y = std::min(zero_y, value_y);
      const int height = std::max(2, std::abs(value_y - zero_y));
      lv_obj_t *bar = lv_obj_create(context->chart);
      context->bars[i] = bar;
      lv_obj_set_size(bar, bar_width, height);
      lv_obj_set_pos(bar, x, y);
      lv_obj_set_style_bg_color(
          bar, bucket.value < 0.0f ? lv_color_make(0xEF, 0x53, 0x50)
                                  : lv_color_make(0x26, 0xA6, 0x9A),
          0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(bar, 0, 0);
      lv_obj_set_style_radius(bar, 3, 0);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }

    const bool show_label = context->week || i % 3 == 0;
    if (!show_label) continue;
    lv_obj_t *label = lv_label_create(context->chart);
    context->axis_labels[i] = label;
    char text[12];
    if (context->week) {
      weekday_text(bucket.start_ms, text, sizeof(text));
    } else {
      std::snprintf(text, sizeof(text), "%02d", i);
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_make(0xB0, 0xB0, 0xB0), 0);
    lv_obj_set_style_text_font(label, ui_font_for_size(11), 0);
    lv_obj_set_width(label, context->week ? bar_width + gap : 28);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, x - (context->week ? gap / 2 : 6),
                   chart_height + 4);
  }
  lv_obj_set_height(context->chart, chart_height + axis_height);
}

void popup_timer_cb(lv_timer_t *timer) {
  update_popup(static_cast<EnergyPopupContext *>(lv_timer_get_user_data(timer)));
}

void close_popup_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *button = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_obj_del(lv_obj_get_parent(lv_obj_get_parent(button)));
}

void popup_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
  auto *context =
      static_cast<EnergyPopupContext *>(lv_event_get_user_data(event));
  if (!context) return;
  if (context->timer) lv_timer_del(context->timer);
  if (g_energy_popup == context) g_energy_popup = nullptr;
  delete context;
}

void period_button_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<EnergyPopupContext *>(lv_event_get_user_data(event));
  if (!context) return;
  context->week =
      lv_event_get_current_target(event) == context->week_button;
  context->rendered_generation = UINT32_MAX;
  energy_request_refresh(context->statistic_id, context->week);
  update_popup(context);
}

void chart_readout_cb(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
  auto *context =
      static_cast<EnergyPopupContext *>(lv_event_get_user_data(event));
  if (!context || !context->chart) return;
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_area_t area;
  lv_obj_get_coords(context->chart, &area);
  const int slots = context->week ? 7 : 24;
  const int relative_x = std::max(
      0, std::min(static_cast<int>(lv_area_get_width(&area)) - 1,
                  static_cast<int>(point.x - area.x1)));
  const int slot = std::max(
      0, std::min(slots - 1,
                  relative_x * slots /
                      std::max(1, static_cast<int>(lv_area_get_width(&area)))));
  EnergySnapshot snapshot;
  if (!energy_get_snapshot(context->statistic_id, snapshot)) return;
  const EnergyBucket &bucket =
      context->week ? snapshot.week[slot] : snapshot.day[slot];
  char period[32];
  if (context->week) {
    weekday_text(bucket.start_ms, period, sizeof(period));
  } else {
    std::snprintf(period, sizeof(period), "%02d:00-%02d:00", slot,
                  (slot + 1) % 24);
  }
  if (!bucket.valid) {
    std::snprintf(period + std::strlen(period),
                  sizeof(period) - std::strlen(period), ": no data");
    lv_label_set_text(context->readout, period);
    return;
  }
  const std::string value = format_energy_value(
      bucket.value, context->decimals, effective_unit(*context, snapshot));
  const std::string text = std::string(period) + ": " + value;
  lv_label_set_text(context->readout, text.c_str());
}

void open_energy_popup(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
  auto *tile_context =
      static_cast<EnergyTileContext *>(lv_event_get_user_data(event));
  if (!tile_context || tile_context->statistic_id.empty()) return;
  if (g_energy_popup != nullptr) return;

  energy_request_refresh(tile_context->statistic_id, true);

  auto *context = new EnergyPopupContext();
  g_energy_popup = context;
  context->statistic_id = tile_context->statistic_id;
  context->title = tile_context->title;
  context->configured_unit = tile_context->configured_unit;
  context->decimals = tile_context->decimals;

  lv_obj_t *overlay = lv_obj_create(lv_layer_top());
  context->overlay = overlay;
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(overlay, popup_delete_cb, LV_EVENT_DELETE, context);

  lv_obj_t *panel = lv_obj_create(overlay);
  lv_obj_set_size(panel, 780, 570);
  lv_obj_set_style_bg_color(panel, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(panel, 18, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 18, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, context->title.c_str());
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(22), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

  lv_obj_t *close = lv_button_create(panel);
  lv_obj_set_size(close, 42, 42);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(close, 0, 0);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -2, -4);
  lv_obj_t *close_label = lv_label_create(close);
  lv_label_set_text(close_label, getMdiChar("close").c_str());
  lv_obj_set_style_text_font(close_label, FONT_MDI_ICONS, 0);
  lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
  lv_obj_center(close_label);
  lv_obj_add_event_cb(close, close_popup_cb, LV_EVENT_CLICKED, nullptr);

  context->value_label = lv_label_create(panel);
  lv_label_set_text(context->value_label, "--");
  lv_obj_set_style_text_color(context->value_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(context->value_label, ui_font_for_size(28), 0);
  lv_obj_align(context->value_label, LV_ALIGN_TOP_LEFT, 4, 40);

  context->status_label = lv_label_create(panel);
  lv_label_set_text(context->status_label, "Loading...");
  lv_obj_set_style_text_color(
      context->status_label, lv_color_make(0xB0, 0xB0, 0xB0), 0);
  lv_obj_set_style_text_font(context->status_label, ui_font_for_size(14), 0);
  lv_obj_align(context->status_label, LV_ALIGN_TOP_LEFT, 4, 76);

  context->day_button = lv_button_create(panel);
  lv_obj_set_size(context->day_button, 90, 38);
  lv_obj_align(context->day_button, LV_ALIGN_TOP_RIGHT, -110, 48);
  lv_obj_t *day_label = lv_label_create(context->day_button);
  lv_label_set_text(day_label, "24H");
  lv_obj_center(day_label);
  lv_obj_add_event_cb(context->day_button, period_button_cb, LV_EVENT_CLICKED,
                      context);

  context->week_button = lv_button_create(panel);
  lv_obj_set_size(context->week_button, 90, 38);
  lv_obj_align(context->week_button, LV_ALIGN_TOP_RIGHT, -12, 48);
  lv_obj_t *week_label = lv_label_create(context->week_button);
  lv_label_set_text(week_label, "7D");
  lv_obj_center(week_label);
  lv_obj_add_event_cb(context->week_button, period_button_cb, LV_EVENT_CLICKED,
                      context);

  context->chart = lv_obj_create(panel);
  lv_obj_set_size(context->chart, 690, 328);
  lv_obj_set_style_bg_color(context->chart, lv_color_make(0x20, 0x20, 0x20), 0);
  lv_obj_set_style_bg_opa(context->chart, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(context->chart, 0, 0);
  lv_obj_set_style_pad_all(context->chart, 0, 0);
  lv_obj_clear_flag(context->chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(context->chart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(context->chart, LV_ALIGN_TOP_MID, 0, 112);
  lv_obj_add_event_cb(context->chart, chart_readout_cb, LV_EVENT_ALL, context);

  context->readout = lv_label_create(panel);
  lv_label_set_text(context->readout, "Touch a bar for details");
  lv_obj_set_style_text_color(context->readout, lv_color_white(), 0);
  lv_obj_set_style_text_font(context->readout, ui_font_for_size(16), 0);
  lv_obj_set_width(context->readout, LV_PCT(100));
  lv_obj_set_style_text_align(context->readout, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(context->readout, LV_ALIGN_BOTTOM_MID, 0, -2);

  context->timer = lv_timer_create(popup_timer_cb, 1000, context);
  update_popup(context);
}

void tile_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
  delete static_cast<EnergyTileContext *>(lv_event_get_user_data(event));
}

}  // namespace

void tile_widget_build_energy(lv_obj_t *parent, const TileData &tile) {
  const std::string statistic_id = tile.entity_id;
  const char *heading =
      tile.title.empty() ? statistic_id.c_str() : tile.title.c_str();

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, heading);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(78));
  lv_obj_set_style_text_color(title, lv_color_make(0xB0, 0xB0, 0xB0), 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(14), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *value = lv_label_create(parent);
  lv_label_set_text(value, "--");
  lv_obj_set_width(value, LV_PCT(100));
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(value, lv_color_white(), 0);
  lv_obj_set_style_text_font(value, energy_value_font(tile.sensor_value_font), 0);
  lv_obj_align(value, LV_ALIGN_CENTER, 0,
               std::max(-100, std::min(200, tile.sensor_value_y_offset)));

  lv_obj_t *unit = lv_label_create(parent);
  lv_label_set_text(unit, tile.sensor_unit.c_str());
  lv_obj_set_style_text_color(unit, lv_color_make(0xB0, 0xB0, 0xB0), 0);
  lv_obj_set_style_text_font(unit, ui_font_for_size(14), 0);
  lv_obj_align(unit, LV_ALIGN_BOTTOM_RIGHT, -2, -2);

  if (!statistic_id.empty()) {
    energy_register_widget(statistic_id, value, unit, tile.sensor_decimals,
                           tile.sensor_unit);
    auto *context = new EnergyTileContext();
    context->statistic_id = statistic_id;
    context->title = tile.title.empty() ? statistic_id : tile.title;
    context->configured_unit = tile.sensor_unit;
    context->decimals =
        tile.sensor_decimals < 0 ? 1 : tile.sensor_decimals;
    const lv_event_code_t open_event =
        tile.popup_open_mode == 0 ? LV_EVENT_LONG_PRESSED
                                  : LV_EVENT_SHORT_CLICKED;
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, open_energy_popup, open_event, context);
    lv_obj_add_event_cb(parent, tile_context_delete_cb, LV_EVENT_DELETE,
                        context);
  }
}

}  // namespace web_admin_local
