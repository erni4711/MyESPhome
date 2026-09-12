// Climate tile: shows current temperature, setpoint, and mode.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_log.h>
#include <lvgl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace web_admin_local {

namespace {

struct ClimateAdjustContext {
  char entity_id[128] = {};
  lv_obj_t *setpoint = nullptr;
  float delta = 0.0f;
};

void climate_adjust_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<ClimateAdjustContext *>(lv_event_get_user_data(event));
  if (!context || !context->setpoint) return;

  char *end = nullptr;
  const float current =
      strtof(lv_label_get_text(context->setpoint), &end);
  if (end == lv_label_get_text(context->setpoint) || !std::isfinite(current)) {
    return;
  }
  const float target = current + context->delta;
  if (!set_home_assistant_climate_temperature(context->entity_id, target)) {
    ESP_LOGW("tile_widget_climate", "Climate setpoint request rejected");
  }
}

void climate_adjust_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<ClimateAdjustContext *>(
        lv_event_get_user_data(event));
  }
}

lv_obj_t *create_adjust_button(lv_obj_t *parent, lv_obj_t *setpoint,
                               const char *text, float delta,
                               const char *entity_id) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_style_bg_color(button, lv_color_make(0x4A, 0x4A, 0x4A), 0);
  lv_obj_set_style_bg_color(button, lv_color_make(0x66, 0x66, 0x66),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_size(button, 34, 34);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, ui_font_for_size(22), 0);
  lv_obj_center(label);

  auto *context = new ClimateAdjustContext{};
  std::snprintf(context->entity_id, sizeof(context->entity_id), "%s",
                entity_id);
  context->setpoint = setpoint;
  context->delta = delta;
  lv_obj_add_event_cb(button, climate_adjust_cb, LV_EVENT_CLICKED, context);
  lv_obj_add_event_cb(button, climate_adjust_context_delete_cb,
                      LV_EVENT_DELETE, context);
  return button;
}

}  // namespace

void tile_widget_build_climate(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  const std::string &entity = tile.entity_id;

  // Title at top-right
  const char *title_text = tile.title.empty() ? (entity.empty() ? "--" : entity.c_str()) : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, title_text);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(70));
  lv_obj_set_style_text_color(title, white, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_RIGHT, 0, 6);

  // Thermostat icon top-left
  lv_obj_t *icon = lv_label_create(parent);
  const std::string icon_char = getMdiChar("thermostat");
  lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xB8, 0x4D), 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 2);

  // Current temperature big in center
  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(40), 0);
  lv_obj_align(temp, LV_ALIGN_CENTER, 0, 6);

  // Setpoint (target temperature) below center
  lv_obj_t *setpoint = lv_label_create(parent);
  lv_label_set_text(setpoint, "--");
  lv_obj_set_style_text_color(setpoint, muted, 0);
  lv_obj_set_style_text_font(setpoint, ui_font_for_size(16), 0);
  lv_obj_align(setpoint, LV_ALIGN_CENTER, 0, 44);

  if (!entity.empty()) {
    lv_obj_t *minus =
        create_adjust_button(parent, setpoint, "-", -0.5f, entity.c_str());
    lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_t *plus =
        create_adjust_button(parent, setpoint, "+", 0.5f, entity.c_str());
    lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
  }

  // Mode / state label at bottom
  lv_obj_t *mode = lv_label_create(parent);
  lv_label_set_text(mode, "—");
  lv_obj_set_style_text_color(mode, muted, 0);
  lv_obj_set_style_text_font(mode, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(mode, LV_PCT(100));
  lv_obj_align(mode, LV_ALIGN_BOTTOM_MID, 0, -4);

  if (!entity.empty()) {
    register_ha_climate_widget(entity, temp, setpoint, mode, icon);
  }
}

}  // namespace web_admin_local
