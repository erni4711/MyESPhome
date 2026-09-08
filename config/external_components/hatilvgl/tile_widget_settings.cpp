// Settings tile and focused display settings overlay.
#include "tiles_lvgl.h"
#include "hatilvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_log.h>
#include <lvgl.h>

namespace web_admin_local {

namespace {

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t y,
                     const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  return label;
}

void settings_back_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_obj_t *button =
        static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    lv_obj_del(lv_obj_get_parent(button));
  }
}

void settings_slider_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  const intptr_t kind = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  const int value = lv_slider_get_value(slider);
  if (kind == 1) {
    settings_set_brightness(static_cast<float>(value) / 100.0f);
  } else {
    settings_set_timeout(static_cast<float>(value));
  }
}

void settings_click_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;

  lv_obj_t *tile = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_obj_t *page = lv_obj_get_parent(tile);
  lv_obj_t *overlay = lv_obj_create(page);
  lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
  lv_obj_center(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_pad_all(overlay, 16, 0);

  make_label(overlay, "Settings", 8, ui_font_for_size(22), lv_color_white());
  make_label(overlay,
             hatilvgl_is_api_connected() ? "Home Assistant: connected"
                                         : "Home Assistant: disconnected",
             48, ui_font_for_size(14),
             hatilvgl_is_api_connected() ? lv_color_make(0x50, 0xD8, 0x88)
                                         : lv_color_make(0xE8, 0x70, 0x70));

  make_label(overlay, "Brightness", 92, ui_font_for_size(14), lv_color_white());
  lv_obj_t *brightness = lv_slider_create(overlay);
  lv_obj_set_width(brightness, lv_pct(82));
  lv_obj_align(brightness, LV_ALIGN_TOP_MID, 0, 118);
  lv_slider_set_range(brightness, 1, 100);
  lv_slider_set_value(brightness, 100, LV_ANIM_OFF);
  lv_obj_add_event_cb(brightness, settings_slider_cb, LV_EVENT_RELEASED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(1)));

  make_label(overlay, "Screen timeout (seconds)", 166, ui_font_for_size(14),
             lv_color_white());
  lv_obj_t *timeout = lv_slider_create(overlay);
  lv_obj_set_width(timeout, lv_pct(82));
  lv_obj_align(timeout, LV_ALIGN_TOP_MID, 0, 192);
  lv_slider_set_range(timeout, 5, 600);
  lv_slider_set_value(timeout, 60, LV_ANIM_OFF);
  lv_obj_add_event_cb(timeout, settings_slider_cb, LV_EVENT_RELEASED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(2)));

  lv_obj_t *back = lv_btn_create(overlay);
  lv_obj_set_size(back, 120, 42);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, "Back");
  lv_obj_center(back_label);
  lv_obj_set_style_text_font(back_label, ui_font_for_size(16), 0);
}

}  // namespace

void tile_widget_build_settings(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);
  const char *title = tile.title.empty() ? "Settings" : tile.title.c_str();

  const bool icon_disabled = isMdiIconDisabled(tile.icon_name);
  const std::string icon_name = tile.icon_name.empty()
                                    ? "cog"
                                    : normalizeMdiIconName(tile.icon_name);
  const std::string icon_char =
      icon_disabled || icon_name.empty() ? "" : getMdiChar(icon_name);

  if (!icon_char.empty()) {
    lv_obj_t *icon = lv_label_create(parent);
    lv_label_set_text(icon, icon_char.c_str());
    lv_obj_set_style_text_color(icon, accent, 0);
    lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);
  }

  lv_obj_t *title_label = lv_label_create(parent);
  lv_label_set_text(title_label, title);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title_label, LV_PCT(90));
  lv_obj_set_style_text_color(title_label, white, 0);
  lv_obj_set_style_text_font(title_label, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 18);

  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, settings_click_cb, LV_EVENT_CLICKED, nullptr);
}

}  // namespace web_admin_local
