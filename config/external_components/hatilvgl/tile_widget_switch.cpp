// Switch / light tile: HomeTiles-style card, toggle, and light controls.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_log.h>
#include <lvgl.h>
#include <cstring>

namespace web_admin_local {

static void switch_toggle_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  auto *context =
      static_cast<SwitchToggleContext *>(lv_event_get_user_data(event));
  auto *switch_obj =
      static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  if (!switch_obj || !context || context->entity_id[0] == '\0') return;

  const bool turn_on = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
  if (!toggle_home_assistant_entity(context->entity_id, turn_on)) {
    if (turn_on) lv_obj_clear_state(switch_obj, LV_STATE_CHECKED);
    else lv_obj_add_state(switch_obj, LV_STATE_CHECKED);
  }
  const char *state =
      lv_obj_has_state(switch_obj, LV_STATE_CHECKED) ? "ON" : "OFF";
  if (context->state_label) lv_label_set_text(context->state_label, state);
}

static void light_brightness_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  auto *context =
      static_cast<SwitchToggleContext *>(lv_event_get_user_data(event));
  auto *slider =
      static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  if (!context || !slider) return;
  const int brightness = lv_slider_get_value(slider);
  if (!set_home_assistant_light_brightness(context->entity_id, brightness)) {
    ESP_LOGW("tile_widget_switch", "Brightness request rejected");
  }
}

static void switch_popup_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) return;
  auto *context =
      static_cast<SwitchToggleContext *>(lv_event_get_user_data(event));
  if (!context) return;
  if (std::strncmp(context->entity_id, "light.", 6) == 0)
    show_light_popup(context->entity_id, context->title);
  else {
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);

    lv_obj_t *panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, LV_PCT(82), 180);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x2A, 0x2A, 0x2A), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *heading = lv_label_create(panel);
    lv_label_set_text(heading, context->title[0] ? context->title
                                                  : context->entity_id);
    lv_obj_set_style_text_color(heading, lv_color_white(), 0);
    lv_obj_set_style_text_font(heading, ui_font_for_size(16), 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 12);

    struct PopupContext {
      char entity_id[128];
      lv_obj_t *overlay;
    };
    auto *popup_context = new PopupContext{};
    std::strncpy(popup_context->entity_id, context->entity_id,
                 sizeof(popup_context->entity_id) - 1);
    popup_context->overlay = overlay;

    auto action_cb = [](lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      auto *popup = static_cast<PopupContext *>(lv_event_get_user_data(e));
      lv_obj_t *button =
          static_cast<lv_obj_t *>(lv_event_get_current_target(e));
      lv_obj_t *label = lv_obj_get_child(button, 0);
      const bool turn_on =
          label != nullptr && std::strcmp(lv_label_get_text(label), "On") == 0;
      toggle_home_assistant_entity(popup->entity_id, turn_on);
    };
    auto close_cb = [](lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      lv_obj_t *button =
          static_cast<lv_obj_t *>(lv_event_get_current_target(e));
      lv_obj_del(lv_obj_get_parent(lv_obj_get_parent(button)));
    };

    for (int i = 0; i < 2; ++i) {
      lv_obj_t *button = lv_button_create(panel);
      lv_obj_set_size(button, 72, 34);
      lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 18 + i * 84, -18);
      lv_obj_t *label = lv_label_create(button);
      lv_label_set_text(label, i == 0 ? "On" : "Off");
      lv_obj_set_style_text_color(label, lv_color_white(), 0);
      lv_obj_center(label);
      lv_obj_add_event_cb(button, action_cb, LV_EVENT_CLICKED, popup_context);
    }

    lv_obj_t *close = lv_button_create(panel);
    lv_obj_set_size(close, 72, 30);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -18, -20);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, popup_context);
    lv_obj_add_event_cb(overlay, [](lv_event_t *e) {
      if (lv_event_get_code(e) == LV_EVENT_DELETE)
        delete static_cast<PopupContext *>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, popup_context);
  }
}

static void switch_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<SwitchToggleContext *>(
        lv_event_get_user_data(event));
  }
}

static SwitchToggleContext *create_switch_context(
    lv_obj_t *state_label, const std::string &entity,
    const std::string &title) {
  auto *context = new SwitchToggleContext{state_label, {}, {}};
  std::strncpy(context->entity_id, entity.c_str(),
               sizeof(context->entity_id) - 1);
  std::strncpy(context->title, title.c_str(), sizeof(context->title) - 1);
  return context;
}

void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0xA0, 0xA0, 0xA0);
  const lv_color_t accent = lv_color_make(0x3B, 0x82, 0xF6);
  const std::string &entity = tile.entity_id;

  if (!tile.title.empty()) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, tile.title.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_color(title, white, 0);
    lv_obj_set_style_text_font(title, ui_font_for_size(16), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 2);
  }

  lv_obj_t *state_label = lv_label_create(parent);
  lv_label_set_text(state_label, "OFF");
  lv_obj_set_style_text_color(state_label, muted, 0);
  lv_obj_set_style_text_font(state_label, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(state_label, LV_PCT(100));
  lv_obj_align(state_label, LV_ALIGN_BOTTOM_MID, 0, -4);

  if (tile.switch_style == 1 && std::strncmp(entity.c_str(), "light.", 6) == 0) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, LV_PCT(82), 16);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_make(0x3B, 0x3B, 0x3B), 0);
    lv_obj_set_style_bg_color(slider, accent, LV_PART_INDICATOR);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);

    auto *context = create_switch_context(state_label, entity, tile.title);
    lv_obj_add_event_cb(slider, light_brightness_cb, LV_EVENT_RELEASED,
                        context);
    lv_obj_add_event_cb(slider, switch_context_delete_cb, LV_EVENT_DELETE,
                        context);
    return;
  }

  lv_obj_t *switch_obj = lv_button_create(parent);
  lv_obj_set_size(switch_obj, LV_PCT(72), 42);
  lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_bg_color(switch_obj, accent,
                            LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(switch_obj, lv_color_make(0x4A, 0x4A, 0x4A),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_radius(switch_obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(switch_obj, 0, 0);
  lv_obj_set_style_shadow_width(switch_obj, 0, 0);
  lv_obj_set_style_pad_all(switch_obj, 0, 0);
  lv_obj_align(switch_obj, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(switch_obj, LV_OBJ_FLAG_CHECKABLE);

  lv_obj_t *caption = lv_label_create(switch_obj);
  lv_label_set_text(caption, "OFF");
  lv_obj_set_style_text_color(caption, white, 0);
  lv_obj_set_style_text_font(caption, ui_font_for_size(16), 0);
  lv_obj_center(caption);

  if (!entity.empty()) {
    auto *context = create_switch_context(state_label, entity, tile.title);
    lv_obj_add_event_cb(switch_obj, switch_toggle_cb,
                        LV_EVENT_VALUE_CHANGED, context);
    lv_obj_add_event_cb(switch_obj, switch_popup_cb,
                        LV_EVENT_LONG_PRESSED, context);
    lv_obj_add_event_cb(switch_obj, switch_context_delete_cb,
                        LV_EVENT_DELETE, context);
    register_ha_switch_widget(entity, switch_obj, state_label);
  }
}

}  // namespace web_admin_local
