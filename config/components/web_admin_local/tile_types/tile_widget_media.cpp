// Media player tile: shows track info + playback controls.
#include "../tiles_lvgl.h"
#include "../web_admin_lvgl_fonts.h"
#include <lvgl.h>
#include <cstring>

namespace web_admin_local {

void tile_widget_build_media(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t muted  = lv_color_make(0x8A, 0x8A, 0x8A);
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);

  // Title / player name at top
  const char *name = tile.title.empty()
                   ? (tile.media_entity.empty() ? "Media" : tile.media_entity.c_str())
                   : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, name);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, muted, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(14), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  // "Now playing" placeholder
  lv_obj_t *track = lv_label_create(parent);
  lv_label_set_text(track, "--");
  lv_obj_set_style_text_color(track, white, 0);
  lv_obj_set_style_text_font(track, ui_font_for_size(14), 0);
  lv_label_set_long_mode(track, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(track, LV_PCT(100));
  lv_obj_align(track, LV_ALIGN_CENTER, 0, -8);

  // Playback buttons row
  lv_obj_t *btn_row = lv_obj_create(parent);
  lv_obj_set_size(btn_row, LV_PCT(100), 36);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_set_style_pad_all(btn_row, 0, 0);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

  const char *btns[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
  for (auto *sym : btns) {
    lv_obj_t *btn = lv_button_create(btn_row);
    lv_obj_set_size(btn, 36, 32);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_bg_color(btn, accent, LV_STATE_PRESSED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, white, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  }
}

}  // namespace web_admin_local
