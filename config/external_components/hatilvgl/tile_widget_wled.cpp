#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"

#include <esp_log.h>
#include <lvgl.h>
#include <src/widgets/dropdown/lv_dropdown.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace web_admin_local {

namespace {

struct WledContext {
  char entity_id[128] = {};
  char preset_entity_id[128] = {};
  char restart_entity_id[128] = {};
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

void brightness_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  auto *slider = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  auto *context = static_cast<WledContext *>(lv_event_get_user_data(event));
  if (!slider || !context) return;
  const int brightness = lv_slider_get_value(slider);
  const bool ok = brightness == 0
                      ? toggle_home_assistant_entity(context->entity_id, false)
                      : set_home_assistant_light_brightness(
                            context->entity_id, brightness);
  if (!ok) {
    ESP_LOGW("tile_widget_wled", "WLED brightness/power request rejected");
  }
}

void preset_cb(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto *dropdown = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    auto *context = static_cast<WledContext *>(lv_event_get_user_data(event));
    if (!dropdown || !context || !context->preset_entity_id[0]) return;
    char option[128] = {};
    lv_dropdown_get_selected_str(dropdown, option, sizeof(option));
    if (!set_home_assistant_select_option(context->preset_entity_id, option))
      ESP_LOGW("tile_widget_wled", "WLED preset request rejected");
}

void restart_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context = static_cast<WledContext *>(lv_event_get_user_data(event));
  if (!context || !context->restart_entity_id[0] ||
      !press_home_assistant_button(context->restart_entity_id))
    ESP_LOGW("tile_widget_wled", "WLED restart request rejected");
}

}  // namespace

void tile_widget_build_wled(lv_obj_t *parent, const TileData &tile) {
  const std::string &entity = tile.entity_id;

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, tile.title.empty() ? (entity.empty() ? "WLED" : entity.c_str())
                                               : tile.title.c_str());
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(72));
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(14), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *brightness = lv_slider_create(parent);
  lv_obj_set_width(brightness, 20);
  lv_obj_set_height(brightness, LV_PCT(82));
  lv_slider_set_range(brightness, 0, 100);
  lv_slider_set_value(brightness, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(brightness, lv_color_make(0xFF, 0xD5, 0x4F),
                            LV_PART_INDICATOR);
  lv_obj_align(brightness, LV_ALIGN_TOP_RIGHT, -4, 28);

  if (!entity.empty()) {
    auto *brightness_context = make_context(entity);
    lv_obj_add_event_cb(brightness, brightness_cb, LV_EVENT_RELEASED,
                        brightness_context);
    lv_obj_add_event_cb(brightness, delete_context_cb, LV_EVENT_DELETE,
                        brightness_context);
    register_ha_wled_widget(entity, brightness);
    if (!tile.wled_restart_entity.empty()) {
      lv_obj_t *restart = lv_button_create(parent);
      lv_obj_set_size(restart, 36, 32);
      lv_obj_align(restart, LV_ALIGN_BOTTOM_LEFT, 0, -4);
      lv_obj_t *label = lv_label_create(restart);
      const std::string restart_icon = getMdiChar("restart");
      lv_label_set_text(label, restart_icon.empty() ? "?" : restart_icon.c_str());
      lv_obj_set_style_text_font(label, FONT_MDI_ICONS, 0);
      lv_obj_set_style_text_color(label, lv_color_white(), 0);
      lv_obj_center(label);
      auto *restart_context = make_context(entity);
      std::snprintf(restart_context->restart_entity_id,
                    sizeof(restart_context->restart_entity_id), "%s",
                    tile.wled_restart_entity.c_str());
      lv_obj_add_event_cb(restart, restart_cb, LV_EVENT_CLICKED,
                          restart_context);
      lv_obj_add_event_cb(restart, delete_context_cb, LV_EVENT_DELETE,
                          restart_context);
    }
    if (!tile.wled_preset_entity.empty()) {
      lv_obj_t *preset = lv_dropdown_create(parent);
      lv_obj_set_width(preset, LV_PCT(68));
      lv_obj_align(preset, LV_ALIGN_TOP_LEFT, 0, 28);
      lv_dropdown_set_options(preset, "");
      auto *preset_context = make_context(entity);
      std::snprintf(preset_context->preset_entity_id,
                    sizeof(preset_context->preset_entity_id), "%s",
                    tile.wled_preset_entity.c_str());
      lv_obj_add_event_cb(preset, preset_cb, LV_EVENT_VALUE_CHANGED,
                          preset_context);
      lv_obj_add_event_cb(preset, delete_context_cb, LV_EVENT_DELETE,
                          preset_context);
      register_ha_wled_preset_widget(tile.wled_preset_entity, preset);
    }
  }
}

}  // namespace web_admin_local
