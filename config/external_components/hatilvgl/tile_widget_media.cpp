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
  lv_obj_t *control_label = nullptr;
};

struct MediaPopupContext {
  char entity_id[128] = {};
  char title[128] = {};
};

struct MediaVolumeContext {
  char entity_id[128] = {};
  lv_obj_t *value_label = nullptr;
};

struct MediaPositionContext {
  char entity_id[128] = {};
  lv_obj_t *value_label = nullptr;
  lv_obj_t *duration_label = nullptr;
  lv_obj_t *state_label = nullptr;
  lv_obj_t *slider = nullptr;
  lv_timer_t *timer = nullptr;
};

void media_command_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<MediaCommandContext *>(lv_event_get_user_data(event));
  if (!context || !context->command) return;
  if (std::strcmp(context->command, "media_toggle_mute") == 0) {
    const bool muted = context->control_label != nullptr &&
                       std::strcmp(lv_label_get_text(context->control_label),
                                   getMdiChar("volume-off").c_str()) == 0;
    if (!set_home_assistant_media_mute(context->entity_id, !muted))
      ESP_LOGW("tile_widget_media", "Media mute request rejected");
    return;
  }
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

void media_volume_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  auto *context =
      static_cast<MediaVolumeContext *>(lv_event_get_user_data(event));
  auto *slider =
      static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  if (!context || !slider) return;
  const int value = lv_slider_get_value(slider);
  if (context->value_label) {
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", value);
    lv_label_set_text(context->value_label, text);
  }
  if (!set_home_assistant_media_volume(context->entity_id,
                                       static_cast<float>(value) / 100.0f)) {
    ESP_LOGW("tile_widget_media", "Media volume request rejected");
  }
}

void media_volume_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE)
    delete static_cast<MediaVolumeContext *>(lv_event_get_user_data(event));
}

void media_position_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
  auto *context =
      static_cast<MediaPositionContext *>(lv_event_get_user_data(event));
  if (!context) return;
  if (context->timer) {
    lv_timer_del(context->timer);
    context->timer = nullptr;
  }
  delete context;
}

void media_position_timer_cb(lv_timer_t *timer) {
  auto *context =
      static_cast<MediaPositionContext *>(lv_timer_get_user_data(timer));
  if (!context || !context->slider || !context->state_label ||
      std::strcmp(lv_label_get_text(context->state_label), "Playing") != 0) {
    return;
  }

  const int position = lv_slider_get_value(context->slider);
  const int duration = lv_slider_get_max_value(context->slider);
  if (position >= duration) return;

  const int next_position = position + 1;
  lv_slider_set_value(context->slider, next_position, LV_ANIM_OFF);
  if (context->value_label) {
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", next_position / 60,
                  next_position % 60);
    lv_label_set_text(context->value_label, text);
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
  context->control_label = label;
  lv_obj_add_event_cb(button, media_command_cb, LV_EVENT_CLICKED, context);
  lv_obj_add_event_cb(button, media_command_context_delete_cb,
                      LV_EVENT_DELETE, context);
  return label;
}

void close_media_popup_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  lv_obj_t *button =
      static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_obj_del(lv_obj_get_parent(button));
}

void media_popup_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
  delete static_cast<MediaPopupContext *>(lv_event_get_user_data(event));
}

void media_popup_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<MediaPopupContext *>(lv_event_get_user_data(event));
  if (!context || context->entity_id[0] == '\0') return;

  lv_obj_t *overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *panel = lv_obj_create(overlay);
  lv_obj_set_size(panel, 520, 520);
  lv_obj_set_style_bg_color(panel, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

  auto *popup_context = new MediaPopupContext(*context);
  lv_obj_add_event_cb(overlay, media_popup_delete_cb, LV_EVENT_DELETE,
                      popup_context);

  lv_obj_t *header_icon = lv_label_create(panel);
  lv_label_set_text(header_icon, getMdiChar("music-box").c_str());
  lv_obj_set_style_text_color(header_icon, lv_color_make(0x26, 0xA6, 0x9A), 0);
  lv_obj_set_style_text_font(header_icon, FONT_MDI_ICONS, 0);
  lv_obj_align(header_icon, LV_ALIGN_TOP_LEFT, 18, 14);

  lv_obj_t *heading = lv_label_create(panel);
  lv_label_set_text(heading, context->title[0] ? context->title : "Media");
  lv_obj_set_style_text_color(heading, lv_color_white(), 0);
  lv_obj_set_style_text_font(heading, ui_font_for_size(16), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 55, 17);

  lv_obj_t *close = lv_button_create(panel);
  lv_obj_set_size(close, 42, 42);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(close, 0, 0);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -8, 7);
  lv_obj_t *close_label = lv_label_create(close);
  lv_label_set_text(close_label, getMdiChar("close").c_str());
  lv_obj_set_style_text_font(close_label, FONT_MDI_ICONS, 0);
  lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
  lv_obj_center(close_label);
  lv_obj_add_event_cb(close, close_media_popup_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *artwork = lv_image_create(panel);
  lv_obj_set_size(artwork, 150, 150);
  lv_obj_align(artwork, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_radius(artwork, 8, 0);
  lv_obj_set_style_clip_corner(artwork, true, 0);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "--");
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(72));
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(20), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 220);

  lv_obj_t *subtitle = lv_label_create(panel);
  lv_label_set_text(subtitle, "--");
  lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
  lv_obj_set_width(subtitle, LV_PCT(82));
  lv_obj_set_style_text_color(subtitle, lv_color_make(0xA0, 0xA0, 0xA0), 0);
  lv_obj_set_style_text_font(subtitle, ui_font_for_size(14), 0);
  lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 250);

  lv_obj_t *state = lv_label_create(panel);
  lv_label_set_text(state, "--");
  lv_obj_set_style_text_color(state, lv_color_make(0xA0, 0xA0, 0xA0), 0);
  lv_obj_set_style_text_font(state, ui_font_for_size(12), 0);
  lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(state, LV_PCT(100));
  lv_obj_align(state, LV_ALIGN_TOP_MID, 0, 302);

  lv_obj_t *controls = lv_obj_create(panel);
  lv_obj_set_size(controls, LV_PCT(70), 42);
  lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(controls, 0, 0);
  lv_obj_set_style_pad_all(controls, 0, 0);
  lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, 335);
  lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *previous = create_media_button(
      controls, popup_context->entity_id, "media_previous_track",
      "skip-previous");
  lv_obj_t *play_pause = create_media_button(
      controls, popup_context->entity_id, "media_play_pause", "play");
  lv_obj_t *next = create_media_button(
      controls, popup_context->entity_id, "media_next_track", "skip-next");
  (void)previous;
  (void)next;

  lv_obj_t *position_row = lv_obj_create(panel);
  lv_obj_set_size(position_row, LV_PCT(82), 36);
  lv_obj_set_style_bg_opa(position_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(position_row, 0, 0);
  lv_obj_set_style_pad_all(position_row, 0, 0);
  lv_obj_set_layout(position_row, LV_LAYOUT_NONE);
  lv_obj_align(position_row, LV_ALIGN_TOP_MID, 0, 388);

  lv_obj_t *position_label = lv_label_create(position_row);
  lv_label_set_text(position_label, "--:--");
  lv_obj_set_style_text_color(position_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(position_label, ui_font_for_size(12), 0);
  lv_obj_align(position_label, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *position_slider = lv_slider_create(position_row);
  lv_obj_set_width(position_slider, 250);
  lv_obj_set_height(position_slider, 12);
  lv_slider_set_range(position_slider, 0, 1);
  lv_slider_set_value(position_slider, 0, LV_ANIM_OFF);
  lv_obj_align(position_slider, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(position_slider, lv_color_make(0x18, 0x3B, 0x52), 0);
  lv_obj_set_style_bg_color(position_slider, lv_color_make(0x26, 0xA6, 0x9A),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(position_slider, lv_color_white(), LV_PART_KNOB);
  lv_obj_add_state(position_slider, LV_STATE_DISABLED);

  lv_obj_t *duration_label = lv_label_create(position_row);
  lv_label_set_text(duration_label, "--:--");
  lv_obj_set_style_text_color(duration_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(duration_label, ui_font_for_size(12), 0);
  lv_obj_align(duration_label, LV_ALIGN_RIGHT_MID, 0, 0);

  auto *position_context = new MediaPositionContext{};
  std::snprintf(position_context->entity_id,
                sizeof(position_context->entity_id), "%s",
                popup_context->entity_id);
  position_context->value_label = position_label;
  position_context->duration_label = duration_label;
  position_context->state_label = state;
  position_context->slider = position_slider;
  lv_obj_add_event_cb(position_slider, media_position_context_delete_cb,
                      LV_EVENT_DELETE, position_context);
  position_context->timer =
      lv_timer_create(media_position_timer_cb, 1000, position_context);

  lv_obj_t *volume_row = lv_obj_create(panel);
  lv_obj_set_size(volume_row, LV_PCT(78), 42);
  lv_obj_set_style_bg_opa(volume_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(volume_row, 0, 0);
  lv_obj_set_style_pad_all(volume_row, 0, 0);
  lv_obj_set_layout(volume_row, LV_LAYOUT_NONE);
  lv_obj_align(volume_row, LV_ALIGN_TOP_MID, 0, 440);

  lv_obj_t *mute_button = create_media_button(
      volume_row, popup_context->entity_id, "media_toggle_mute", "volume-high");
  lv_obj_t *mute_button_obj = lv_obj_get_parent(mute_button);
  lv_obj_align(mute_button_obj, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *volume_slider = lv_slider_create(volume_row);
  lv_obj_set_width(volume_slider, 190);
  lv_obj_set_height(volume_slider, 14);
  lv_slider_set_range(volume_slider, 0, 100);
  lv_slider_set_value(volume_slider, 50, LV_ANIM_OFF);
  lv_obj_align(volume_slider, LV_ALIGN_CENTER, 10, 0);
  lv_obj_set_style_bg_color(volume_slider, lv_color_make(0x18, 0x3B, 0x52), 0);
  lv_obj_set_style_bg_color(volume_slider, lv_color_make(0x26, 0xA6, 0x9A),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volume_slider, lv_color_white(),
                            LV_PART_KNOB);

  lv_obj_t *volume_label = lv_label_create(volume_row);
  lv_label_set_text(volume_label, "50%");
  lv_obj_set_style_text_color(volume_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(volume_label, ui_font_for_size(13), 0);
  lv_obj_align(volume_label, LV_ALIGN_RIGHT_MID, 0, 0);

  auto *volume_context = new MediaVolumeContext{};
  std::snprintf(volume_context->entity_id, sizeof(volume_context->entity_id),
                "%s", popup_context->entity_id);
  volume_context->value_label = volume_label;
  lv_obj_add_event_cb(volume_slider, media_volume_cb, LV_EVENT_RELEASED,
                      volume_context);
  lv_obj_add_event_cb(volume_slider, media_volume_context_delete_cb,
                      LV_EVENT_DELETE, volume_context);

  register_ha_media_widget(popup_context->entity_id, title, subtitle, state,
                           play_pause, header_icon, artwork, volume_slider,
                           volume_label, mute_button, position_slider,
                           position_label, duration_label);
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
  lv_obj_t *artwork = lv_image_create(parent);
  lv_obj_set_size(artwork, LV_PCT(50), LV_PCT(50));
  lv_obj_set_style_pad_all(artwork, 0, 0);
  lv_obj_align(artwork, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(artwork, LV_OBJ_FLAG_HIDDEN);

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
    register_ha_media_widget(entity, title, subtitle, state, play_pause, icon,
                             artwork);
    auto *popup_context = new MediaPopupContext{};
    std::snprintf(popup_context->entity_id, sizeof(popup_context->entity_id),
                  "%s", entity.c_str());
    std::snprintf(popup_context->title, sizeof(popup_context->title), "%s",
                  name);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, media_popup_cb, LV_EVENT_CLICKED,
                        popup_context);
    lv_obj_add_event_cb(parent, media_popup_delete_cb, LV_EVENT_DELETE,
                        popup_context);
  }
}

}  // namespace web_admin_local
