// Navigate tile: tapping navigates to another folder page.
#include "tiles_lvgl.h"
#include <lvgl.h>
#include <cstdint>

namespace web_admin_local {

static void navigate_tile_click_cb(lv_event_t *event) {
  if (event == nullptr || g_tiles_renderer == nullptr) return;
  const int target_folder =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
  if (target_folder < 0 || target_folder > 9) return;
  g_tiles_renderer->show_folder(target_folder);
}

void tile_widget_build_navigate(lv_obj_t *parent, const TileData &tile, int /*folder_id*/) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Arrow icon at centre-top
  lv_obj_t *arrow = lv_label_create(parent);
  lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(arrow, lv_color_make(0x26, 0xA6, 0x9A), 0);
  lv_obj_set_style_text_font(arrow, ui_font_for_size(14), 0);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, 0, 0);

  // Folder name
  const char *name = tile.title.empty() ? "Folder" : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, ui_font_for_size(14), 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

  // "Tap to enter" hint
  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "Tap to open");
  lv_obj_set_style_text_color(hint, muted, 0);
  lv_obj_set_style_text_font(hint, ui_font_for_size(14), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, navigate_tile_click_cb, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(tile.navigate_target)));
}

}  // namespace web_admin_local
