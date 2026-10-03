// Climate tile: shows current temperature, setpoint, and mode.
#include "tiles_lvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_log.h>
#include <lvgl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if 0
#undef ESP_LOGE
#undef ESP_LOGD
#undef ESP_LOGI
#undef ESP_LOGW

#define ESP_LOGE(t, m, ...) printf("[%s:%u] " m "\n", t, __LINE__, ##__VA_ARGS__)
#define ESP_LOGD(t, m, ...) printf("[%s:%u] " m "\n", t, __LINE__, ##__VA_ARGS__)
#define ESP_LOGI(t, m, ...) printf("[%s:%u] " m "\n", t, __LINE__, ##__VA_ARGS__)
#define ESP_LOGW(t, m, ...) printf("[%s:%u] " m "\n", t, __LINE__, ##__VA_ARGS__)
#endif
namespace web_admin_local {

namespace {
static const char *TAG = "tile_widget_climate";
struct ClimateAdjustContext {
  char entity_id[128] = {};
  lv_obj_t *setpoint = nullptr;
  float delta = 0.0f;
};

struct ClimateArcContext {
  char entity_id[128] = {};
};

struct ClimateModeContext {
  char entity_id[128] = {};
};

struct ClimatePresetContext {
  char entity_id[128] = {};
  float temperature = 0.0f;
  const char *name = nullptr;
};

void climate_preset_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<ClimatePresetContext *>(lv_event_get_user_data(event));
  if (!context) return;
  ESP_LOGI(TAG, "Climate preset: entity=%s name=%s temperature=%.1f",
           context->entity_id, context->name, context->temperature);
  if (!set_home_assistant_climate_temperature(context->entity_id,
                                              context->temperature)) {
    ESP_LOGW(TAG, "Climate preset rejected: entity=%s name=%s",
             context->entity_id, context->name);
  }
}

void climate_preset_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<ClimatePresetContext *>(lv_event_get_user_data(event));
  }
}

lv_obj_t *create_climate_preset_button(lv_obj_t *parent, const char *entity_id,
                                       const char *text, float temperature,
                                       lv_align_t alignment, lv_coord_t x_offset,
                                       lv_coord_t y_offset) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, 62, 26);
  lv_obj_align(button, alignment, x_offset, y_offset);
  lv_obj_set_style_bg_color(button, lv_color_make(0x4A, 0x4A, 0x4A), 0);
  lv_obj_set_style_bg_color(button, lv_color_make(0x66, 0x66, 0x66),
                            LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, 6, 0);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, ui_font_for_size(12), 0);
  lv_obj_center(label);

  auto *context = new ClimatePresetContext{};
  std::snprintf(context->entity_id, sizeof(context->entity_id), "%s",
                entity_id);
  context->temperature = temperature;
  context->name = text;
  lv_obj_add_event_cb(button, climate_preset_cb, LV_EVENT_CLICKED, context);
  lv_obj_add_event_cb(button, climate_preset_context_delete_cb, LV_EVENT_DELETE,
                      context);
  return button;
}

void climate_icon_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<ClimateModeContext *>(lv_event_get_user_data(event));
  auto *icon = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  if (!context || !icon) return;

  const std::string off_icon = getMdiChar("radiator-off");
  const bool is_off = std::strcmp(lv_label_get_text(icon), off_icon.c_str()) == 0;
  const char *mode = is_off ? "heat" : "off";
  ESP_LOGI(TAG, "Climate icon toggle: entity=%s mode=%s",
           context->entity_id, mode);
  if (!set_home_assistant_climate_mode(context->entity_id, mode)) {
    ESP_LOGW(TAG, "Climate icon toggle rejected: entity=%s mode=%s",
             context->entity_id, mode);
  }
}

void climate_icon_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<ClimateModeContext *>(lv_event_get_user_data(event));
  }
}

void climate_adjust_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<ClimateAdjustContext *>(lv_event_get_user_data(event));
  if (!context || !context->setpoint) return;

  char *end = nullptr;
  const float current =
      strtof(lv_label_get_text(context->setpoint), &end);
  if (end == lv_label_get_text(context->setpoint) || !std::isfinite(current)) {
    ESP_LOGW(TAG, "Button adjustment ignored: invalid setpoint text for %s",
             context->entity_id);
    return;
  }
  const float target = current + context->delta;
  ESP_LOGI(TAG, "Button adjustment: entity=%s current=%.1f delta=%.1f target=%.1f",
           context->entity_id, current, context->delta, target);
  if (!set_home_assistant_climate_temperature(context->entity_id, target)) {
    ESP_LOGW(TAG, "Climate setpoint request rejected");
  } else {
    ESP_LOGI(TAG, "Button adjustment accepted: entity=%s target=%.1f",
             context->entity_id, target);
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
                             LV_STATE_PRESSED);
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

void climate_arc_value_changed_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  auto *context =
      static_cast<ClimateArcContext *>(lv_event_get_user_data(event));
  auto *arc = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  if (!context || !arc) return;
  const float target = static_cast<float>(std::lround(
      static_cast<float>(lv_arc_get_value(arc)) / 5.0f)) / 2.0f;
  ESP_LOGI(TAG, "Arc released: entity=%s raw=%d target=%.1f",
           context->entity_id, lv_arc_get_value(arc), target);
  if (!set_home_assistant_climate_temperature(context->entity_id, target)) {
    ESP_LOGW(TAG, "Climate arc request rejected");
  } else {
    ESP_LOGI(TAG, "Arc request accepted: entity=%s target=%.1f",
             context->entity_id, target);
  }
}

void climate_arc_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<ClimateArcContext *>(
        lv_event_get_user_data(event));
  }
}

lv_obj_t *create_climate_arc(lv_obj_t *parent, const char *entity_id,
                             int diameter) {
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_set_size(arc, diameter, diameter);
  lv_obj_align(arc, LV_ALIGN_CENTER, 0, 4);
  lv_arc_set_range(arc, 50, 350);
  lv_arc_set_value(arc, 200);
  lv_arc_set_rotation(arc, 140);
  lv_arc_set_bg_angles(arc, 0, 260);
  lv_arc_set_change_rate(arc, 1000);
  ESP_LOGI(TAG, "Climate arc created: entity=%s diameter=%d range=5.0..35.0",
           entity_id, diameter);
  int width = diameter / 10;
  lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_make(0x4A, 0x4A, 0x4A),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_make(0xFF, 0xB8, 0x4D),
                             LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(arc, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_border_width(arc, 5, LV_PART_KNOB);
  lv_obj_set_style_border_color(arc, lv_color_make(0xFF, 0xB8, 0x4D),
                                LV_PART_KNOB);

  auto *context = new ClimateArcContext{};
  std::snprintf(context->entity_id, sizeof(context->entity_id), "%s",
                entity_id);
  // Let LVGL redraw the knob continuously while dragging. Send the service
  // request only after release so network I/O cannot interrupt the redraw.
  lv_obj_add_event_cb(arc, climate_arc_value_changed_cb,
                      LV_EVENT_RELEASED, context);
  lv_obj_add_event_cb(arc, climate_arc_context_delete_cb,
                      LV_EVENT_DELETE, context);
  return arc;
}

void climate_mode_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto *context =
      static_cast<ClimateModeContext *>(lv_event_get_user_data(event));
  auto *button = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  if (!context || !button || lv_obj_get_child_count(button) == 0) return;

  static constexpr const char *kHvacModes[] = {
      "off", "heat", "cool", "heat_cool", "auto", "dry", "fan_only",
  };
  lv_obj_t *label = lv_obj_get_child(button, 0);
  const char *current = lv_label_get_text(label);
  size_t next = 0;
  for (size_t i = 0; i < sizeof(kHvacModes) / sizeof(kHvacModes[0]); ++i) {
    if (std::strcmp(current, kHvacModes[i]) == 0) {
      next = (i + 1) % (sizeof(kHvacModes) / sizeof(kHvacModes[0]));
      break;
    }
  }
  const char *mode = kHvacModes[next];
  lv_label_set_text(label, mode);
  ESP_LOGI(TAG, "Climate mode request: entity=%s mode=%s",
           context->entity_id, mode);
  if (!set_home_assistant_climate_mode(context->entity_id, mode)) {
    ESP_LOGW(TAG, "Climate mode request rejected");
  }
}

void climate_mode_context_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete static_cast<ClimateModeContext *>(
        lv_event_get_user_data(event));
  }
}

lv_obj_t *create_climate_mode_button(lv_obj_t *parent, lv_obj_t *mode,
                                     const char *entity_id) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_width(button, 96);
  lv_obj_set_height(button, 32);
  lv_obj_set_style_bg_color(button, lv_color_make(0x4A, 0x4A, 0x4A), 0);
  lv_obj_set_style_bg_color(button, lv_color_make(0x66, 0x66, 0x66),
                             LV_STATE_PRESSED);
  lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -4);

  lv_obj_set_parent(mode, button);
  lv_obj_center(mode);

  auto *context = new ClimateModeContext{};
  std::snprintf(context->entity_id, sizeof(context->entity_id), "%s",
                entity_id);
  lv_obj_add_event_cb(button, climate_mode_cb, LV_EVENT_CLICKED, context);
  lv_obj_add_event_cb(button, climate_mode_context_delete_cb,
                      LV_EVENT_DELETE, context);
  return button;
}

}  // namespace
int tile_climate_arc_diameter(const GridGeometry &geo, const TileData::Geometry &tile)
{
  return std::min(geo.tile_w(tile.span_w), geo.tile_h(tile.span_h)) - 40;
}

void tile_climate_arc_position_for_temperature(int radius, float temperature, int &x, int &y)
{
  const float ratio = std::max(0.0f, std::min(1.0f,
      (temperature - 5.0f) / 30.0f));
  const float angle = (140.0f + ratio * 260.0f) * 0.01745329252f;
  radius -= 6;
  x = static_cast<int>(radius * std::cos(angle));
  y = static_cast<int>(radius * std::sin(angle));
}

void tile_widget_build_climate(lv_obj_t *parent, const TileData &tile, const GridGeometry &geo) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  const std::string &entity = tile.entity_id;
  const bool large_climate_tile = tile.geometry.span_w > 1 && tile.geometry.span_h > 1;
  lv_obj_t *target_arc = nullptr;
  lv_obj_t *current_marker = nullptr;
  lv_obj_t *mode_button = nullptr;

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

  auto tile_geo = tile.geometry;
  int w = geo.tile_w(tile_geo.span_w);
  int h = geo.tile_h(tile_geo.span_h);
  ESP_LOGI(TAG, "Building climate tile: entity=%s large=%d width=%d height=%d",
           entity.c_str(), large_climate_tile, w,h);
  if (large_climate_tile && !entity.empty()) {

    const int diameter = tile_climate_arc_diameter(geo, tile_geo);
    ESP_LOGI(TAG, "Creating climate arc: entity=%s diameter=%d",
             entity.c_str(), diameter);
    target_arc = create_climate_arc(parent, entity.c_str(), diameter);
    current_marker = lv_obj_create(parent);
    lv_obj_remove_style_all(current_marker);
    lv_obj_set_size(current_marker, diameter/10-2, diameter/10-2);
    lv_obj_set_style_radius(current_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(current_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(current_marker, 6, 0);
    lv_obj_set_style_border_color(current_marker,
                                  lv_color_make(0xFF, 0, 0), 0); // 0xB8, 0x4D
    lv_obj_add_flag(current_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(current_marker, target_arc, LV_ALIGN_CENTER, 0, 0);
    ESP_LOGI(TAG,
             "Climate marker created: entity=%s marker=%p parent=%p size=%dx%d "
             "pos=%d,%d hidden=%d",
             entity.c_str(), static_cast<void *>(current_marker),
             static_cast<void *>(parent),
             static_cast<int>(lv_obj_get_width(current_marker)),
             static_cast<int>(lv_obj_get_height(current_marker)),
             static_cast<int>(lv_obj_get_x(current_marker)),
             static_cast<int>(lv_obj_get_y(current_marker)),
             lv_obj_has_flag(current_marker, LV_OBJ_FLAG_HIDDEN));
  }

  // Thermostat icon top-left
  lv_obj_t *icon = lv_label_create(parent);
  const std::string icon_char = getMdiChar("thermostat");
  lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xB8, 0x4D), 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 2);
  if (!entity.empty()) {
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    auto *icon_context = new ClimateModeContext{};
    std::snprintf(icon_context->entity_id, sizeof(icon_context->entity_id), "%s",
                  entity.c_str());
    lv_obj_add_event_cb(icon, climate_icon_cb, LV_EVENT_CLICKED, icon_context);
    lv_obj_add_event_cb(icon, climate_icon_context_delete_cb, LV_EVENT_DELETE,
                        icon_context);
  }

  // Current temperature big in center
  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, ui_font_for_size(40), 0);
  lv_obj_align(temp, LV_ALIGN_CENTER, 0, 0);

  // Setpoint (target temperature) below center
  lv_obj_t *setpoint = lv_label_create(parent);
  lv_label_set_text(setpoint, "--");
  lv_obj_set_style_text_color(setpoint, muted, 0);
  lv_obj_set_style_text_font(setpoint, ui_font_for_size(16), 0);
  lv_obj_align(setpoint, LV_ALIGN_CENTER, 0, 30);

  // HVAC mode toggle button at the bottom
  lv_obj_t *mode = lv_label_create(parent);
  lv_label_set_text(mode, "--");
  lv_obj_set_style_text_color(mode, muted, 0);
  lv_obj_set_style_text_font(mode, ui_font_for_size(14), 0);
  if (!entity.empty()) {
    //mode_button = create_climate_mode_button(parent, mode, entity.c_str());
  }

  if (!entity.empty()) {
    lv_obj_t *minus =
        create_adjust_button(parent, setpoint, "-", -0.5f, entity.c_str());
    lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_t *plus =
        create_adjust_button(parent, setpoint, "+", 0.5f, entity.c_str());
    lv_obj_align(plus, LV_ALIGN_TOP_RIGHT, 0, 44);
    create_climate_preset_button(parent, entity.c_str(), "16°", 16.0f,
                                 LV_ALIGN_BOTTOM_LEFT, 0, 0);
    create_climate_preset_button(parent, entity.c_str(), "21°", 21.0f,
                                 LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    TileData::Geometry tile_geometry;
    tile_geometry.col = tile.geometry.col;
    tile_geometry.row = tile.geometry.row;
    tile_geometry.span_w = tile.geometry.span_w;
    tile_geometry.span_h = tile.geometry.span_h;
    register_ha_climate_widget(entity, tile_geometry, temp, setpoint, mode,
                                icon, minus, plus, target_arc, current_marker,
                                mode_button);
  }

}

}  // namespace web_admin_local
