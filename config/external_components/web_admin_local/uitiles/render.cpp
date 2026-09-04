#include "config/components/uitiles/render.h"
#include <cstdio>

namespace uitiles {

Render::Render(lv_obj_t* parent) : parent_(parent) {}

Render::~Render() { clear(); }

void Render::clear() {
  for (auto obj : tile_objs_) {
    if (obj) lv_obj_del(obj);
  }
  tile_objs_.clear();
}

void Render::createTileObject(const Tile& tile, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
  if (!parent_) return;
  lv_obj_t* cont = lv_obj_create(parent_);
  tile_objs_.push_back(cont);
  lv_obj_set_size(cont, w, h);
  lv_obj_set_pos(cont, x, y);

  // Simple style: background color if provided
  lv_obj_set_style_pad_all(cont, 6, 0);
  lv_obj_set_style_radius(cont, 6, 0);
  lv_obj_set_style_bg_color(cont, lv_color_make((tile.bg_color >> 16) & 0xFF, (tile.bg_color >> 8) & 0xFF, tile.bg_color & 0xFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(cont, tile.background_opacity == 255 ? LV_OPA_COVER : (lv_opa_t)((tile.background_opacity * 255) / 255), LV_PART_MAIN);

  // Title label at top
  lv_obj_t* label = lv_label_create(cont);
  lv_label_set_text(label, tile.title.c_str());
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 6);

  // Icon name (if any) as small label under title
  if (tile.icon_name.length() > 0) {
    lv_obj_t* icon = lv_label_create(cont);
    lv_label_set_text(icon, tile.icon_name.c_str());
    lv_obj_set_style_text_font(icon, LV_FONT_DEFAULT, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 6, 26);
  }

  // Content for sensor tiles: show entity id
  if (tile.type == TILE_SENSOR) {
    lv_obj_t* val = lv_label_create(cont);
    String text = tile.sensor_entity.length() > 0 ? tile.sensor_entity : String("-");
    lv_label_set_text(val, text.c_str());
    lv_obj_align(val, LV_ALIGN_CENTER, 0, 0);
  }

  // For scene/other tiles show a simple marker
  if (tile.type == TILE_SCENE) {
    lv_obj_t* btn = lv_btn_create(cont);
    lv_obj_set_size(btn, w/2, h/3);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t* b_label = lv_label_create(btn);
    lv_label_set_text(b_label, "Run");
    lv_obj_center(b_label);
  }
}

void Render::renderGrid(const std::vector<Tile>& grid) {
  clear();
  if (!parent_) return;

  lv_coord_t parent_w = lv_obj_get_width(parent_);
  lv_coord_t parent_h = lv_obj_get_height(parent_);
  if (parent_w <= 0 || parent_h <= 0) {
    // cannot compute layout; bail
    return;
  }

  const int cols = GRID_COLS;
  const int rows = GRID_ROWS;

  // compute cell sizes
  int total_gap_w = (cols - 1) * GRID_GAP;
  int total_gap_h = (rows - 1) * GRID_GAP;
  int avail_w = parent_w - 2 * GRID_PAD - total_gap_w;
  int avail_h = parent_h - 2 * GRID_PAD - total_gap_h;
  if (avail_w <= 0 || avail_h <= 0) return;
  int cell_w = avail_w / cols;
  int cell_h = avail_h / rows;

  for (size_t i = 0; i < grid.size() && i < (size_t)(cols * rows); ++i) {
    const Tile& t = grid[i];
    uint8_t col = t.col;
    uint8_t row = t.row;
    uint8_t span_w = t.span_w < 1 ? 1 : t.span_w;
    uint8_t span_h = t.span_h < 1 ? 1 : t.span_h;
    if (col >= (uint8_t)cols) col = i % cols;
    if (row >= (uint8_t)rows) row = i / cols;

    lv_coord_t x = GRID_PAD + col * (cell_w + GRID_GAP);
    lv_coord_t y = GRID_PAD + row * (cell_h + GRID_GAP);
    lv_coord_t w = cell_w * span_w + GRID_GAP * (span_w - 1);
    lv_coord_t h = cell_h * span_h + GRID_GAP * (span_h - 1);

    createTileObject(t, x, y, w, h);
  }
}

} // namespace uitiles
