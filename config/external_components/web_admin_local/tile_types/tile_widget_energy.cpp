// Energy / power tile — re-uses sensor layout.
// This file is intentionally minimal: build_sensor already handles TILE_ENERGY
// because the TilesLvglRenderer dispatches both TILE_SENSOR and TILE_ENERGY to
// tile_widget_build_sensor().  This file provides the stub forward declaration
// so that the linker is satisfied.
#include "../tiles_lvgl.h"
#include <lvgl.h>

// tile_widget_build_sensor declared in tile_widget_sensor.cpp
// No separate implementation needed for energy — handled by shared sensor widget.
