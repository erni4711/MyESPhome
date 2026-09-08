// Media-player tile: now-playing information and Home Assistant controls.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_log.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace web_admin_local {

namespace {

struct MediaCommandContext {
  char entity_id[128] = {};
  const char *command = nullptr;
};

void media_command_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<MediaCommandContext *>(lv_event_get_user_data(event));
  if (!context || !context->command) return;
  if (!call_home_assistant_media_command(context->entity_id,
                                         context->command)) {
    ESP_LOGW("tile_widget_media", "Media command request rejected");
  }
}

void media_command_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<MediaCommandContext *>(
        lv_event_get_user_data(event));
  }
}

lv_obj_t *create_media_button(lv_obj_t *parent, const char *entity_id,
                              const char *command, const char *icon_name) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, 38, 34);
  lv_obj_set_style_bg_color(button, lv_color_make(0x33, 0x33, 0x33), 0);
  lv_obj_set_style_bg_color(button, lv_color_make(0x26, 0xA6, 0x9A),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_all(button, 0, 0);

  lv_obj_t *label = lv_label_create(button);
  const std::string icon = getMdiChar(icon_name);
  lv_label_set_text(label, icon.empty() ? "?" : icon.c_str());
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, FONT_MDI_ICONS, 0);
  lv_obj_center(label);

  auto *context = new MediaCommandContext{};
  std::snprintf(context->entity_id, sizeof(context->entity_id), "%s",
                entity_id);
  context->command = command;
  lv_obj_add_event_cb(button, media_command_cb, LV_EVENT_CLICKED, context);
  lv_obj_add_event_cb(button, media_command_context_delete_cb,
                      LV_EVENT_DELETE, context);
  return label;
}

}  // namespace

void tile_widget_build_media(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0xA0, 0xA0, 0xA0);
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);
  const std::string &entity = tile.entity_id;

  // HomeTiles-style media header: icon left, player name right.
  lv_obj_t *icon = lv_label_create(parent);
  const std::string icon_char = getMdiChar("television");
  lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
  lv_obj_set_style_text_color(icon, accent, 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

  const char *name = tile.title.empty()
                         ? (entity.empty() ? "Media" : entity.c_str())
                         : tile.title.c_str();
  lv_obj_t *name_label = lv_label_create(parent);
  lv_label_set_text(name_label, name);
  lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name_label, LV_PCT(76));
  lv_obj_set_style_text_color(name_label, muted, 0);
  lv_obj_set_style_text_font(name_label, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(name_label, LV_ALIGN_TOP_RIGHT, 0, 4);

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "--");
  lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, white, 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(17), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -12);

  lv_obj_t *subtitle = lv_label_create(parent);
  lv_label_set_text(subtitle, "--");
  lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
  lv_obj_set_width(subtitle, LV_PCT(100));
  lv_obj_set_style_text_color(subtitle, muted, 0);
  lv_obj_set_style_text_font(subtitle, ui_font_for_size(13), 0);
  lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 12);

  lv_obj_t *state = lv_label_create(parent);
  lv_label_set_text(state, "--");
  lv_obj_set_style_text_color(state, muted, 0);
  lv_obj_set_style_text_font(state, ui_font_for_size(12), 0);
  lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(state, LV_PCT(100));
  lv_obj_align(state, LV_ALIGN_BOTTOM_MID, 0, -38);

  lv_obj_t *controls = lv_obj_create(parent);
  lv_obj_set_size(controls, LV_PCT(100), 36);
  lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(controls, 0, 0);
  lv_obj_set_style_pad_all(controls, 0, 0);
  lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *previous = nullptr;
  lv_obj_t *play_pause = nullptr;
  lv_obj_t *next = nullptr;
  if (!entity.empty()) {
    previous = create_media_button(controls, entity.c_str(),
                                   "media_previous_track", "skip-previous");
    play_pause = create_media_button(controls, entity.c_str(),
                                     "media_play_pause", "play");
    next = create_media_button(controls, entity.c_str(),
                               "media_next_track", "skip-next");
  }
  (void)previous;
  (void)next;

  if (!entity.empty()) {
    register_ha_media_widget(entity, title, subtitle, state, play_pause, icon);
  }
}

}  // namespace web_admin_local
