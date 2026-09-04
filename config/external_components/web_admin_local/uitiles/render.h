#ifndef UI_TILES_RENDER_H
#define UI_TILES_RENDER_H

#include "config/components/uitiles/model.h"
#include <lvgl.h>
#include <vector>

namespace uitiles {

static constexpr int GRID_GAP = 8; // px
static constexpr int GRID_PAD = 8; // px

class Render {
 public:
  // parent: lvgl object to render into; model: optional model pointer
  explicit Render(lv_obj_t* parent);
  ~Render();

  // Render the provided grid into the parent. Clears previous contents.
  void renderGrid(const std::vector<Tile>& grid);

  // Remove previously created LVGL objects
  void clear();

  // Change parent
  void setParent(lv_obj_t* parent) { parent_ = parent; }

 private:
  lv_obj_t* parent_;
  std::vector<lv_obj_t*> tile_objs_;

  // Helpers
  void createTileObject(const Tile& tile, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
};

} // namespace uitiles

#endif // UI_TILES_RENDER_H
