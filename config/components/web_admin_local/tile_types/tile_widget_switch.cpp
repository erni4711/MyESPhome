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
  if (context->state_label) lv_label_set_text(context->state_label, state);
}

void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
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
  lv_obj_set_style_text_font(title, ui_font_for_size(14), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *switch_obj = nullptr;
  if (tile.switch_style == 1) {
    // Brightness slider style
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, LV_PCT(85), 14);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(0x3B, 0x82, 0xF6), LV_PART_INDICATOR);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
  } else {
    // Toggle switch
    switch_obj = lv_switch_create(parent);
    lv_obj_set_size(switch_obj, 56, 28);
    lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x3B, 0x82, 0xF6), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(switch_obj, LV_ALIGN_CENTER, 0, 0);
    auto *context = new SwitchToggleContext{nullptr, {}};
    std::strncpy(context->entity_id, entity.c_str(), sizeof(context->entity_id) - 1);
    context->entity_id[sizeof(context->entity_id) - 1] = '\0';
    lv_obj_add_event_cb(switch_obj, switch_toggle_cb, LV_EVENT_VALUE_CHANGED, context);
  }

  // State label at bottom
  lv_obj_t *state_lbl = lv_label_create(parent);
  lv_label_set_text(state_lbl, "OFF");
  lv_obj_set_style_text_color(state_lbl, muted, 0);
  lv_obj_set_style_text_font(state_lbl, ui_font_for_size(14), 0);
  lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  if (!entity.empty()) register_ha_switch_widget(entity, switch_obj, state_lbl);
}

}  // namespace web_admin_local
