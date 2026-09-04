// Clock tile: shows HH:MM time and optional date.
#include "../tiles_lvgl.h"
#include <lvgl.h>
#include <ctime>
#include <cstdio>

namespace web_admin_local {

static const lv_font_t *clock_font(int raw_size, int fallback) {
  const int size = raw_size >= 96 ? 96 : raw_size >= 80 ? 80 :
                   raw_size >= 72 ? 72 : raw_size >= 64 ? 64 :
                   raw_size >= 56 ? 56 : raw_size >= 48 ? 48 :
                   raw_size >= 40 ? 40 : raw_size >= 32 ? 32 :
                   raw_size >= 28 ? 28 : raw_size >= 24 ? 24 :
                   raw_size >= 20 ? 20 : fallback;
  return ui_font_for_size(static_cast<uint8_t>(size));
}

static void format_clock_time(char *buf, size_t size, const tm &tm_info,
                              int format) {
  if (format == 2) {
    int hour = tm_info.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(buf, size, "%d:%02d %s", hour, tm_info.tm_min,
             tm_info.tm_hour < 12 ? "AM" : "PM");
    return;
  }
  strftime(buf, size, "%H:%M", &tm_info);
}

static lv_obj_t *clock_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color,
                             bool shadow) {
  if (shadow) {
    lv_obj_t *shadow_label = lv_label_create(parent);
    lv_label_set_text(shadow_label, text);
    lv_obj_set_style_text_color(shadow_label, lv_color_black(), 0);
    lv_obj_set_style_text_opa(shadow_label, LV_OPA_50, 0);
    lv_obj_set_style_text_font(shadow_label, font, 0);
    lv_obj_align(shadow_label, LV_ALIGN_CENTER, 2, 2);
  }
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_font(label, font, 0);
  return label;
}

struct ClockTimerContext {
  lv_obj_t *label;
  lv_obj_t *date_label;
  lv_timer_t *timer;
  int format;
  int date_format;
  bool show_weekday;
};

static void clock_parent_delete_cb(lv_event_t *event) {
  auto *context = static_cast<ClockTimerContext *>(lv_event_get_user_data(event));
  if (context == nullptr) return;
  if (context->timer != nullptr) {
    lv_timer_delete(context->timer);
    context->timer = nullptr;
  }
  delete context;
}

static void format_clock_date(char *buf, size_t size, const tm &tm_info,
                              int format, bool show_weekday) {
  const char *date_format = (format == 2) ? "%m/%d/%Y"
                           : (format == 3) ? "%Y/%m/%d" : "%d.%m.%Y";
  char date[32];
  strftime(date, sizeof(date), date_format, &tm_info);
  if (show_weekday) {
    char weekday[16];
    strftime(weekday, sizeof(weekday), "%A", &tm_info);
    snprintf(buf, size, "%s, %s", weekday, date);
  } else {
    snprintf(buf, size, "%s", date);
  }
}

static void clock_update_cb(lv_event_t *e) {
  lv_obj_t *time_lbl = (lv_obj_t *)lv_event_get_user_data(e);
  if (!time_lbl) return;
  time_t now = time(nullptr);
  struct tm tm_info;
  localtime_r(&now, &tm_info);
  char buf[16];
  format_clock_time(buf, sizeof(buf), tm_info, 0);
  lv_label_set_text(time_lbl, buf);
}

void tile_widget_build_clock(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);
  bool show_time = (tile.clock_flags & 1) != 0;
  bool show_date = (tile.clock_flags & 2) != 0;
  ClockTimerContext *timer_context = nullptr;

  // Title (optional)
  if (!tile.title.empty()) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, tile.title.c_str());
    lv_obj_set_style_text_color(lbl, muted, 0);
    lv_obj_set_style_text_font(lbl, ui_font_for_size(14), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Time HH:MM
  if (show_time) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char tbuf[16];
    format_clock_time(tbuf, sizeof(tbuf), tm_info, tile.clock_time_format);

    // Time font size from key_code (stored as clock time font size, default 40)
    const lv_font_t *font = clock_font(tile.key_code, 40);
    lv_obj_t *time_lbl = clock_label(parent, tbuf, font, white, tile.clock_shadow);
    const lv_text_align_t alignment = tile.clock_time_alignment == 0
        ? LV_TEXT_ALIGN_LEFT : tile.clock_time_alignment == 2
        ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
    lv_obj_set_style_text_align(time_lbl, alignment, 0);
    int y_offset = show_date ? -12 : 0;
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, y_offset);

    // Register a 1-second timer to keep the display updated
    timer_context = new ClockTimerContext{time_lbl, nullptr, nullptr,
                                          tile.clock_time_format,
                                          tile.clock_date_format, tile.clock_show_weekday};
    timer_context->timer = lv_timer_create([](lv_timer_t *timer) {
      auto *context = static_cast<ClockTimerContext *>(lv_timer_get_user_data(timer));
      if (!context || !context->label || !lv_obj_is_valid(context->label)) {
        if (context != nullptr) {
          context->timer = nullptr;
        }
        lv_timer_delete(timer);
        return;
      }
      time_t n = time(nullptr);
      struct tm tm;
      localtime_r(&n, &tm);
      char b[16];
      format_clock_time(b, sizeof(b), tm, context->format);
      lv_label_set_text(context->label, b);
      if (context->date_label) {
        char date[48];
        format_clock_date(date, sizeof(date), tm, context->date_format,
                          context->show_weekday);
        lv_label_set_text(context->date_label, date);
      }
    }, 1000, timer_context);
    lv_obj_add_event_cb(parent, clock_parent_delete_cb, LV_EVENT_DELETE,
                        timer_context);
  }

  // Date
  if (show_date) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char dbuf[48];
    format_clock_date(dbuf, sizeof(dbuf), tm_info, tile.clock_date_format,
                      tile.clock_show_weekday);

    lv_obj_t *date_lbl = clock_label(parent, dbuf, clock_font(tile.key_modifier, 20),
                                      lv_color_make(0xCC, 0xCC, 0xCC),
                                      tile.clock_shadow);
    const lv_text_align_t alignment = tile.clock_date_alignment == 0
        ? LV_TEXT_ALIGN_LEFT : tile.clock_date_alignment == 2
        ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
    lv_obj_set_style_text_align(date_lbl, alignment, 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, show_time ? 22 : 0);
    if (show_time) {
      timer_context->date_label = date_lbl;
    }
  }
}

}  // namespace web_admin_local
