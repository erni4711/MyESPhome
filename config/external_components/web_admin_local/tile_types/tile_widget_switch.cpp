// Switch / Light tile: toggle button or brightness slider.
#include "../tiles_lvgl.h"
#include <lvgl.h>
#include <cstring>

namespace web_admin_local {

static void switch_toggle_cb(lv_event_t *e) {
  auto *context = static_cast<SwitchToggleContext *>(lv_event_get_user_data(e));
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  if (!sw || context == nullptr) return;
  const bool turn_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (!toggle_home_assistant_entity(context->entity_id, turn_on)) {
    if (turn_on) lv_obj_clear_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  const char *state = lv_obj_has_state(sw, LV_STATE_CHECKED) ? "ON" : "OFF";
  lv_obj_t *caption = lv_obj_get_child(sw, 0);
  if (caption) lv_label_set_text(caption, state);
  if (context->state_label) lv_label_set_text(context->state_label, state);
}

static void switch_popup_cb(lv_event_t *e) {
  auto *context = static_cast<SwitchToggleContext *>(lv_event_get_user_data(e));
  if (context && std::strncmp(context->entity_id, "light.", 6) == 0) {
    show_light_popup(context->entity_id, context->title);
  }
}

void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);
  const std::string &entity = tile.switch_entity.empty()
                              ? tile.sensor_entity : tile.switch_entity;

  // Title label
  const char *heading = tile.title.empty() ? entity.c_str() : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, heading);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, muted, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(16), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *switch_obj = nullptr;
  if (tile.switch_style == 1) {
    // Brightness slider style. Brightness commands are not available in the
    // local Home Assistant REST adapter yet.
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, LV_PCT(85), 14);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(0x3B, 0x82, 0xF6), LV_PART_INDICATOR);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
  } else {
    // Toggle button, matching the HomeTiles standard switch tile.
    switch_obj = lv_button_create(parent);
    lv_obj_set_size(switch_obj, LV_PCT(70), 36);
    lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x2A, 0x2A, 0x2A), 0);
    lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x3B, 0x82, 0xF6),
                              LV_STATE_CHECKED);
    lv_obj_set_style_radius(switch_obj, 8, 0);
    lv_obj_align(switch_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(switch_obj, LV_OBJ_FLAG_CHECKABLE);
  }

  if (switch_obj != nullptr) {
    lv_obj_t *caption = lv_label_create(switch_obj);
    lv_label_set_text(caption, "OFF");
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, 0);
  }

  lv_obj_t *state_lbl = lv_label_create(parent);
  lv_label_set_text(state_lbl, "OFF");
  lv_obj_set_style_text_color(state_lbl, muted, 0);
  lv_obj_set_style_text_font(state_lbl, ui_font_for_size(16), 0);
  lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  if (switch_obj != nullptr) {
    auto *context = new SwitchToggleContext{state_lbl, {}, {}};
    std::strncpy(context->entity_id, entity.c_str(), sizeof(context->entity_id) - 1);
    context->entity_id[sizeof(context->entity_id) - 1] = '\0';
    std::strncpy(context->title, tile.title.c_str(), sizeof(context->title) - 1);
    context->title[sizeof(context->title) - 1] = '\0';
    lv_obj_add_event_cb(switch_obj, switch_toggle_cb, LV_EVENT_VALUE_CHANGED, context);
    lv_obj_add_event_cb(switch_obj, switch_popup_cb, LV_EVENT_LONG_PRESSED, context);
    if (!entity.empty()) register_ha_switch_widget(entity, switch_obj, state_lbl);
  }
}

}  // namespace web_admin_local
