#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"

#include <esp_log.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace web_admin_local {

namespace {

struct WledContext {
  char entity_id[128] = {};
};

WledContext *make_context(const std::string &entity_id) {
  auto *context = new WledContext{};
  std::snprintf(context->entity_id, sizeof(context->entity_id), "%s",
                entity_id.c_str());
  return context;
}

void delete_context_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE)
    delete static_cast<WledContext *>(lv_event_get_user_data(event));
}

void power_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *button = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  auto *context = static_cast<WledContext *>(lv_event_get_user_data(event));
  if (!button || !context) return;
  const bool turn_on = !lv_obj_has_state(button, LV_STATE_CHECKED);
  if (!toggle_home_assistant_entity(context->entity_id, turn_on)) {
    ESP_LOGW("tile_widget_wled", "WLED power request rejected");
    return;
  }
  if (turn_on) lv_obj_add_state(button, LV_STATE_CHECKED);
  else lv_obj_clear_state(button, LV_STATE_CHECKED);
  lv_obj_t *label = lv_obj_get_child(button, 0);
  if (label) lv_label_set_text(label, turn_on ? "ON" : "OFF");
}

void brightness_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  auto *slider = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  auto *context = static_cast<WledContext *>(lv_event_get_user_data(event));
  if (!slider || !context) return;
  if (!set_home_assistant_light_brightness(context->entity_id,
                                            lv_slider_get_value(slider))) {
    ESP_LOGW("tile_widget_wled", "WLED brightness request rejected");
  }
}

struct WledColorContext : WledContext {
  lv_obj_t *red = nullptr;
  lv_obj_t *green = nullptr;
  lv_obj_t *blue = nullptr;
};

void color_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE)
    delete static_cast<WledColorContext *>(lv_event_get_user_data(event));
}

void rgb_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  auto *context =
      static_cast<WledColorContext *>(lv_event_get_user_data(event));
  if (!context || !context->red || !context->green || !context->blue) return;
  if (!set_home_assistant_light_rgb(
          context->entity_id, lv_slider_get_value(context->red),
          lv_slider_get_value(context->green), lv_slider_get_value(context->blue))) {
    ESP_LOGW("tile_widget_wled", "WLED color request rejected");
  }
}

void effect_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *button = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  auto *context = static_cast<WledContext *>(lv_event_get_user_data(event));
  if (!button || !context) return;
  static const char *effects[] = {"Solid", "Rainbow", "Fire"};
  lv_obj_t *label = lv_obj_get_child(button, 0);
  const char *current = label ? lv_label_get_text(label) : effects[0];
  size_t index = 0;
  for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); ++i) {
    if (std::strcmp(current, effects[i]) == 0) {
      index = (i + 1) % (sizeof(effects) / sizeof(effects[0]));
      break;
    }
  }
  const char *effect = effects[index];
  if (label) lv_label_set_text(label, effect);
  if (!set_home_assistant_light_effect(context->entity_id, effect)) {
    ESP_LOGW("tile_widget_wled", "WLED effect request rejected");
  }
}

lv_obj_t *make_slider(lv_obj_t *parent, lv_color_t color, int y) {
  lv_obj_t *slider = lv_slider_create(parent);
  lv_obj_set_width(slider, LV_PCT(58));
  lv_obj_set_height(slider, 10);
  lv_slider_set_range(slider, 0, 255);
  lv_slider_set_value(slider, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, color, LV_PART_INDICATOR);
  lv_obj_align(slider, LV_ALIGN_TOP_RIGHT, -4, y);
  return slider;
}

}  // namespace

void tile_widget_build_wled(lv_obj_t *parent, const TileData &tile) {
  const std::string &entity = tile.entity_id;
  const lv_color_t muted = lv_color_make(0xB0, 0xB0, 0xB0);

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, tile.title.empty() ? (entity.empty() ? "WLED" : entity.c_str())
                                               : tile.title.c_str());
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(72));
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(14), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *power = lv_button_create(parent);
  lv_obj_set_size(power, 48, 28);
  lv_obj_align(power, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_t *power_label = lv_label_create(power);
  lv_label_set_text(power_label, "OFF");
  lv_obj_center(power_label);

  lv_obj_t *brightness = lv_slider_create(parent);
  lv_obj_set_width(brightness, LV_PCT(58));
  lv_obj_set_height(brightness, 10);
  lv_slider_set_range(brightness, 0, 100);
  lv_slider_set_value(brightness, 100, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(brightness, lv_color_make(0xFF, 0xD5, 0x4F),
                            LV_PART_INDICATOR);
  lv_obj_align(brightness, LV_ALIGN_TOP_RIGHT, -4, 38);

  const char *names[] = {"R", "G", "B"};
  const lv_color_t colors[] = {
      lv_color_make(0xEF, 0x44, 0x44), lv_color_make(0x22, 0xC5, 0x5E),
      lv_color_make(0x3B, 0x82, 0xF6)};
  lv_obj_t *rgb[3] = {};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, names[i]);
    lv_obj_set_style_text_color(label, muted, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 58 + i * 20);
    rgb[i] = make_slider(parent, colors[i], 60 + i * 20);
  }

  lv_obj_t *effect_button = lv_button_create(parent);
  lv_obj_set_width(effect_button, LV_PCT(70));
  lv_obj_align(effect_button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_t *effect = lv_label_create(effect_button);
  lv_label_set_text(effect, "Solid");
  lv_obj_center(effect);

  if (!entity.empty()) {
    auto *power_context = make_context(entity);
    lv_obj_add_event_cb(power, power_cb, LV_EVENT_CLICKED, power_context);
    lv_obj_add_event_cb(power, delete_context_cb, LV_EVENT_DELETE, power_context);

    auto *brightness_context = make_context(entity);
    lv_obj_add_event_cb(brightness, brightness_cb, LV_EVENT_RELEASED,
                        brightness_context);
    lv_obj_add_event_cb(brightness, delete_context_cb, LV_EVENT_DELETE,
                        brightness_context);

    auto *color_context = new WledColorContext{};
    std::snprintf(color_context->entity_id, sizeof(color_context->entity_id), "%s",
                  entity.c_str());
    color_context->red = rgb[0];
    color_context->green = rgb[1];
    color_context->blue = rgb[2];
    for (lv_obj_t *slider : rgb) {
      lv_obj_add_event_cb(slider, rgb_cb, LV_EVENT_RELEASED, color_context);
    }
    lv_obj_add_event_cb(parent, color_context_delete_cb, LV_EVENT_DELETE,
                        color_context);

    auto *effect_context = make_context(entity);
    lv_obj_add_event_cb(effect_button, effect_cb, LV_EVENT_CLICKED, effect_context);
    lv_obj_add_event_cb(effect_button, delete_context_cb, LV_EVENT_DELETE,
                        effect_context);
    register_ha_wled_widget(entity, power, brightness, rgb[0], rgb[1], rgb[2],
                            effect);
  }
}

}  // namespace web_admin_local
