#include "tiles_lvgl.h"

#include <lvgl.h>

namespace hatidart {
bool is_available() __attribute__((weak));
void start_game() __attribute__((weak));
}  // namespace hatidart

namespace web_admin_local {

namespace {

void dart_tile_click_cb(lv_event_t *event) {
  if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  if (hatidart::is_available != nullptr && hatidart::is_available()) {
    hatidart::start_game();
  }
}

lv_obj_t *make_board_ring(lv_obj_t *parent, lv_coord_t size, lv_color_t color) {
  lv_obj_t *ring = lv_obj_create(parent);
  lv_obj_set_size(ring, size, size);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ring, color, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ring, 0, 0);
  lv_obj_set_style_pad_all(ring, 0, 0);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_EVENT_BUBBLE);
  return ring;
}

}  // namespace

void tile_widget_build_dart(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t red = lv_color_make(0xC8, 0x32, 0x32);
  const lv_color_t green = lv_color_make(0x28, 0x8A, 0x55);
  const lv_color_t black = lv_color_make(0x16, 0x16, 0x16);
  const lv_color_t cream = lv_color_make(0xE8, 0xD0, 0x86);

  // Concentric rings provide a lightweight board illustration without a bitmap.
  make_board_ring(parent, 86, black);
  make_board_ring(parent, 72, cream);
  make_board_ring(parent, 56, red);
  make_board_ring(parent, 42, black);
  make_board_ring(parent, 28, green);
  make_board_ring(parent, 12, red);

  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, tile.title.empty() ? "Darts" : tile.title.c_str());
  lv_obj_set_width(label, LV_PCT(90));
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, dart_tile_click_cb, LV_EVENT_CLICKED, nullptr);
}

}  // namespace web_admin_local
