// Sensor / Energy tile: shows entity value, unit, optional gauge.
#include "../tiles_lvgl.h"
#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace web_admin_local {

void tile_widget_build_sensor(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t muted  = lv_color_make(0x8A, 0x8A, 0x8A);

  bool is_energy = (tile.type == TILE_ENERGY);
  const std::string &entity = is_energy ? tile.energy_entity : tile.sensor_entity;

  // Title / entity label at top
  const char *heading = tile.title.empty() ? entity.c_str() : tile.title.c_str();
  lv_obj_t *title_lbl = lv_label_create(parent);
  lv_label_set_text(title_lbl, heading);
  lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title_lbl, LV_PCT(100));
  lv_obj_set_style_text_color(title_lbl, muted, 0);
  lv_obj_set_style_text_font(title_lbl, ui_font_for_size(14), 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  // Value label in the center (placeholder — live value updated by HA bridge)
  char val_buf[32];
  snprintf(val_buf, sizeof(val_buf), "--");
  lv_obj_t *val_lbl = lv_label_create(parent);
  lv_label_set_text(val_lbl, val_buf);
  lv_obj_set_style_text_color(val_lbl, white, 0);
  lv_obj_set_style_text_font(val_lbl, ui_font_for_size(28), 0);
  lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, 0);

  // Unit label below value
  if (!tile.sensor_unit.empty()) {
    lv_obj_t *unit_lbl = lv_label_create(parent);
    lv_label_set_text(unit_lbl, tile.sensor_unit.c_str());
    lv_obj_set_style_text_color(unit_lbl, muted, 0);
    lv_obj_set_style_text_font(unit_lbl, ui_font_for_size(14), 0);
    lv_obj_align(unit_lbl, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
  }

  // Optional gauge arc (display_mode == 1)
  lv_obj_t *arc = nullptr;
  if (tile.sensor_display_mode == 1) {
    arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 60, 60);
    lv_arc_set_range(arc, (int)tile.sensor_gauge_min, (int)tile.sensor_gauge_max);
    lv_arc_set_value(arc, (int)tile.sensor_gauge_min);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, lv_color_make(0x26, 0xA6, 0x9A), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_align(arc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  // Live updates: Home Assistant `state_changed` events (see
  // ha_ws_client.cpp) refresh val_lbl/arc from the ESPHome loop() task.
  if (!entity.empty()) {
    register_ha_entity_widget(entity, val_lbl, arc, tile.sensor_decimals,
                               tile.sensor_gauge_min, tile.sensor_gauge_max);
  }
}

}  // namespace web_admin_local
