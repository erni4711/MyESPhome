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
  char title[128] = {};
  lv_obj_t *image = nullptr;
};

void close_camera_popup_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
  if (overlay != nullptr) lv_obj_del(overlay);
}

void camera_click_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<CameraEventContext *>(lv_event_get_user_data(event));
  if (!context || context->entity_id[0] == '\0') return;

  lv_obj_t *overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_pad_all(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(overlay, close_camera_popup_cb, LV_EVENT_CLICKED,
                      overlay);

  lv_obj_t *image = lv_image_create(overlay);
  lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(image, 0, 0);
  lv_obj_align(image, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(image, LV_OBJ_FLAG_CLICKABLE);
  if (context->image != nullptr) {
    const void *source = lv_image_get_src(context->image);
    if (source != nullptr) {
      lv_image_set_src(image, source);
      lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_obj_add_event_cb(image, close_camera_popup_cb, LV_EVENT_CLICKED, overlay);

  lv_obj_t *heading = lv_label_create(overlay);
  lv_label_set_text(heading, context->title[0] ? context->title
                                               : context->entity_id);
  lv_obj_set_style_text_color(heading, lv_color_white(), 0);
  lv_obj_set_style_text_font(heading, ui_font_for_size(16), 0);
  lv_obj_set_style_bg_color(heading, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(heading, LV_OPA_50, 0);
  lv_obj_set_style_pad_all(heading, 8, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 12);

  lv_obj_t *close = lv_button_create(overlay);
  lv_obj_set_size(close, 48, 48);
  lv_obj_set_style_bg_color(close, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(close, LV_OPA_50, 0);
  lv_obj_set_style_border_width(close, 0, 0);
  lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -12, 12);
  lv_obj_t *close_label = lv_label_create(close);
  lv_label_set_text(close_label, getMdiChar("close").c_str());
  lv_obj_set_style_text_font(close_label, FONT_MDI_ICONS, 0);
  lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
  lv_obj_center(close_label);
  lv_obj_add_event_cb(close, close_camera_popup_cb, LV_EVENT_CLICKED, overlay);

  register_ha_camera_widget(context->entity_id, image, nullptr);
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

  lv_obj_t *image = lv_image_create(parent);
  lv_obj_set_size(image, LV_PCT(100), LV_PCT(72));
  lv_obj_set_style_pad_all(image, 0, 0);
  lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);

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
  lv_label_set_text(state, entity.empty() ? "Unavailable" : "Connecting...");
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
    std::strncpy(context->title, name, sizeof(context->title) - 1);
    context->image = image;
    lv_obj_add_event_cb(parent, camera_click_cb, LV_EVENT_CLICKED, context);
    lv_obj_add_event_cb(parent, camera_context_delete_cb, LV_EVENT_DELETE,
                        context);
    register_ha_camera_widget(entity, image, state);
  }
}

}  // namespace web_admin_local
