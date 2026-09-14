// Camera tile: HomeTiles-style camera card with live Home Assistant state.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_log.h>
#include <lvgl.h>
#include <cstring>

namespace web_admin_local {

namespace {

struct CameraEventContext {
  char entity_id[128] = {};
};

void camera_click_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<CameraEventContext *>(lv_event_get_user_data(event));
  if (!context || context->entity_id[0] == '\0') return;

  // Camera streaming/popups require a display-capable camera integration.
  ESP_LOGI("tile_widget_camera",
           "Camera tile clicked for %s; camera popup is not available",
           context->entity_id);
}

void camera_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<CameraEventContext *>(
        lv_event_get_user_data(event));
  }
}

}  // namespace

void tile_widget_build_camera(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0xA0, 0xA0, 0xA0);
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);
  const std::string &entity = tile.entity_id;

  lv_obj_t *icon = lv_label_create(parent);
  const bool icon_disabled = isMdiIconDisabled(tile.icon_name);
  const std::string icon_name = tile.icon_name.empty()
                                    ? "video"
                                    : normalizeMdiIconName(tile.icon_name);
  const std::string icon_char =
      icon_disabled || icon_name.empty() ? "" : getMdiChar(icon_name);
  lv_label_set_text(icon, icon_char.c_str());
  lv_obj_set_style_text_color(icon, accent, 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);

  const char *name = tile.title.empty()
                         ? (entity.empty() ? "Camera" : entity.c_str())
                         : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, name);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(90));
  lv_obj_set_style_text_color(title, white, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 18);

  lv_obj_t *state = lv_label_create(parent);
  lv_label_set_text(state, entity.empty() ? "Unavailable" : "Idle");
  lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
  lv_obj_set_width(state, LV_PCT(90));
  lv_obj_set_style_text_color(state, muted, 0);
  lv_obj_set_style_text_font(state, ui_font_for_size(12), 0);
  lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(state, LV_ALIGN_BOTTOM_MID, 0, -2);

  if (!entity.empty()) {
    auto *context = new CameraEventContext{};
    std::strncpy(context->entity_id, entity.c_str(),
                 sizeof(context->entity_id) - 1);
    lv_obj_add_event_cb(parent, camera_click_cb, LV_EVENT_CLICKED, context);
    lv_obj_add_event_cb(parent, camera_context_delete_cb, LV_EVENT_DELETE,
                        context);
    register_ha_entity_widget(entity, state, nullptr, -1, 0.f, 100.f);
    register_ha_entity_icon(entity, icon);
  }
}

}  // namespace web_admin_local
