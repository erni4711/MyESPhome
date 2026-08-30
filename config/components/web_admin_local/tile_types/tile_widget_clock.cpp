// Clock tile: shows HH:MM time and optional date.
#include "../tiles_lvgl.h"
#include <lvgl.h>
#include <ctime>
#include <cstdio>

namespace web_admin_local {

static void clock_update_cb(lv_event_t *e) {
  lv_obj_t *time_lbl = (lv_obj_t *)lv_event_get_user_data(e);
  if (!time_lbl) return;
  time_t now = time(nullptr);
  struct tm tm_info;
  localtime_r(&now, &tm_info);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &tm_info);
  lv_label_set_text(time_lbl, buf);
}

void tile_widget_build_clock(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);
  bool show_time = (tile.clock_flags & 1) != 0;
  bool show_date = (tile.clock_flags & 2) != 0;

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
    strftime(tbuf, sizeof(tbuf), "%H:%M", &tm_info);

    lv_obj_t *time_lbl = lv_label_create(parent);
    lv_label_set_text(time_lbl, tbuf);
    lv_obj_set_style_text_color(time_lbl, white, 0);
    // Time font size from key_code (stored as clock time font size, default 40)
    const lv_font_t *font = ui_font_for_size(static_cast<uint8_t>(
        tile.key_code >= 48 ? 48 : tile.key_code >= 36 ? 40 : tile.key_code >= 28 ? 28 : 20));
    lv_obj_set_style_text_font(time_lbl, font, 0);
    int y_offset = show_date ? -12 : 0;
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, y_offset);

    // Register a 1-second timer to keep the display updated
    lv_timer_create([](lv_timer_t *timer) {
      lv_obj_t *lbl = (lv_obj_t *)lv_timer_get_user_data(timer);
      if (!lbl || !lv_obj_is_valid(lbl)) {
        lv_timer_delete(timer);
        return;
      }
      time_t n = time(nullptr);
      struct tm tm;
      localtime_r(&n, &tm);
      char b[16];
      strftime(b, sizeof(b), "%H:%M", &tm);
      lv_label_set_text(lbl, b);
    }, 1000, time_lbl);
  }

  // Date
  if (show_date) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char dbuf[32];
    // Format based on clock_date_format: 0=auto, 1=DD.MM.YYYY, 2=MM/DD/YYYY
    const char *fmt = (tile.clock_date_format == 2) ? "%m/%d/%Y"
                    : (tile.clock_date_format == 3) ? "%Y/%m/%d"
                    : "%d.%m.%Y";
    strftime(dbuf, sizeof(dbuf), fmt, &tm_info);

    lv_obj_t *date_lbl = lv_label_create(parent);
    lv_label_set_text(date_lbl, dbuf);
    lv_obj_set_style_text_color(date_lbl, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_text_font(date_lbl, ui_font_for_size(14), 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, show_time ? 22 : 0);
  }
}

}  // namespace web_admin_local
