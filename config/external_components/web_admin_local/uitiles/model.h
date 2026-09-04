#ifndef UI_TILES_MODEL_H
#define UI_TILES_MODEL_H

#include "esphome.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <vector>
#include <stdint.h>

namespace uitiles {

// Default grid dimensions; can be overridden by device-specific configuration.
static constexpr uint8_t GRID_COLS = 4;
static constexpr uint8_t GRID_ROWS = 4;
static constexpr size_t TILES_PER_GRID = static_cast<size_t>(GRID_COLS) * GRID_ROWS;

// Tile types mirror the HomeTiles definitions so persisted/remote formats remain compatible.
enum TileType : uint8_t {
  TILE_EMPTY = 0,
  TILE_SENSOR = 1,
  TILE_SCENE = 2,
  TILE_KEY = 3,      // retired; numeric value kept for stored configurations
  TILE_FOLDER = 4,
  TILE_SWITCH = 5,
  TILE_IMAGE = 6,    // retired; numeric value kept for stored configurations
  TILE_SETTINGS = 7,
  TILE_BACK = 8,
  TILE_CLOCK = 9,
  TILE_TEXT = 10,
  TILE_COUNTER = 11, // retired; numeric value kept for stored configurations
  TILE_WEATHER = 12,
  TILE_RADAR = 13,   // retired; numeric value kept for stored configurations
  TILE_ENERGY = 14,
  TILE_MEDIA = 15,
  TILE_PIXELANIM = 16,
  TILE_CLIMATE = 17,
  TILE_CAMERA = 18,
  TILE_COVER = 19
};

static inline bool is_retired_tile_type(TileType type) {
  return type == TILE_KEY || type == TILE_IMAGE || type == TILE_COUNTER || type == TILE_RADAR;
}

enum TilePopupOpenMode : uint8_t {
  TILE_POPUP_OPEN_LONG_PRESS = 0,
  TILE_POPUP_OPEN_SHORT_PRESS = 1
};

// A minimal Tile datamodel
struct Tile {
  TileType type;
  String title;
  String icon_name;
  uint32_t bg_color;
  uint8_t background_opacity;  // 0..255

  uint8_t col;
  uint8_t row;
  uint8_t span_w;
  uint8_t span_h;

  String sensor_entity;
  String sensor_unit;
  uint8_t sensor_decimals;
  uint8_t sensor_value_font;
  uint8_t sensor_display_mode;
  int32_t sensor_gauge_min;
  int32_t sensor_gauge_max;
  uint16_t sensor_gauge_arc;
  uint16_t sensor_gauge_size;
  int16_t sensor_gauge_y_offset;
  int16_t sensor_value_y_offset;
  uint16_t sensor_graph_height;
  uint8_t popup_open_mode;

  String scene_alias;

  String key_macro;
  uint8_t key_code;
  uint8_t key_modifier;

  String image_path;
  uint16_t image_slideshow_sec;

  Tile()
      : type(TILE_EMPTY),
        bg_color(0),
        background_opacity(255),
        col(0),
        row(0),
        span_w(1),
        span_h(1),
        sensor_decimals(0xFF),
        sensor_value_font(0),
        sensor_display_mode(0),
        sensor_gauge_min(0),
        sensor_gauge_max(100),
        sensor_gauge_arc(100),
        sensor_gauge_size(350),
        sensor_gauge_y_offset(12),
        sensor_value_y_offset(0),
        sensor_graph_height(60),
        popup_open_mode(TILE_POPUP_OPEN_LONG_PRESS),
        key_code(0),
        key_modifier(0),
        image_slideshow_sec(10) {}
};

// Media tile span constraints
static constexpr uint8_t MEDIA_TILE_MIN_SPAN = 2;
static constexpr uint8_t MEDIA_TILE_MAX_SPAN = 3;

static inline void clamp_media_tile_span(TileType type, uint8_t& span_w, uint8_t& span_h) {
  if (type != TILE_MEDIA) return;
  const uint8_t min_w = GRID_COLS >= MEDIA_TILE_MIN_SPAN ? MEDIA_TILE_MIN_SPAN : GRID_COLS;
  const uint8_t min_h = GRID_ROWS >= MEDIA_TILE_MIN_SPAN ? MEDIA_TILE_MIN_SPAN : GRID_ROWS;
  if (span_w < min_w) span_w = min_w;
  if (span_h < min_h) span_h = min_h;
  if (span_w > MEDIA_TILE_MAX_SPAN) span_w = MEDIA_TILE_MAX_SPAN;
  if (span_h > MEDIA_TILE_MAX_SPAN) span_h = MEDIA_TILE_MAX_SPAN;
}

static inline void clamp_media_tile_layout(TileType type, uint8_t& col, uint8_t& row, uint8_t& span_w, uint8_t& span_h) {
  clamp_media_tile_span(type, span_w, span_h);
  if (type != TILE_MEDIA) return;
  if (span_w > GRID_COLS) span_w = GRID_COLS;
  if (span_h > GRID_ROWS) span_h = GRID_ROWS;
  if (col > GRID_COLS - span_w) col = GRID_COLS - span_w;
  if (row > GRID_ROWS - span_h) row = GRID_ROWS - span_h;
}

// JSON helpers (declarations only)
void tileToJsonObject(const Tile& tile, JsonObject& obj);
bool tileFromJsonObject(const JsonObject& obj, Tile& tile);

// Binary packed V7 types (declarations)
static constexpr uint8_t PACKED_GRID_VERSION = 7;
static constexpr size_t TITLE_MAX     = 32;
static constexpr size_t ICON_MAX      = 32;
static constexpr size_t ENTITY_MAX    = 64;
static constexpr size_t UNIT_MAX      = 16;
static constexpr size_t SCENE_MAX     = 32;
static constexpr size_t MACRO_MAX     = 32;

static_assert(TILES_PER_QUARTER == 4, "TILES_PER_QUARTER must be 4");

struct PackedTileV7 {
  uint8_t type;
  uint8_t sensor_decimals;
  uint8_t key_code;
  uint8_t key_modifier;
  uint32_t bg_color;
  uint8_t col;
  uint8_t row;
  uint8_t span_w;
  uint8_t span_h;
  char title[TITLE_MAX];
  char icon_name[ICON_MAX];
  char sensor_entity[ENTITY_MAX];
  char sensor_unit[UNIT_MAX];
  char scene_alias[SCENE_MAX];
  char key_macro[MACRO_MAX];
  uint8_t sensor_value_font;
  uint16_t image_slideshow_sec;
  uint8_t sensor_gauge_enabled;
  int32_t sensor_gauge_min;
  int32_t sensor_gauge_max;
  uint8_t popup_open_mode;
  uint8_t reserved[3];
};

struct PackedQuarterGridV7 {
  uint8_t version;
  uint8_t quarter_index;
  uint8_t reserved[2];
  PackedTileV7 tiles[TILES_PER_QUARTER];
};

// UiTilesModel class declaration
class UiTilesModel {
 public:
  explicit UiTilesModel(fs::FS& storage_fs);
  ~UiTilesModel() = default;

  // JSON IO
  bool saveGridToJsonFile(const String& path, const std::vector<Tile>& grid);
  bool loadGridFromJsonFile(const String& path, std::vector<Tile>& out_grid);

  // Binary V7 IO
  bool saveGridToBinaryFile(uint16_t folder_id, const std::vector<Tile>& grid);
  bool loadGridFromBinaryFile(uint16_t folder_id, std::vector<Tile>& out_grid);

  // Path helpers
  static String tileGridFile(uint16_t folder_id);
  static String imagePathFile(uint16_t folder_id, size_t index);
  static String entityPathFile(uint16_t folder_id, size_t index);

 private:
  fs::FS& fs_;

  // Internal helpers
  static void copy_cstr_from_String(const String& src, char* dst, size_t max_len);
  static String cstr_to_String(const char* src);

  bool writePackedGridFile(uint16_t folder_id, const PackedQuarterGridV7* packed, size_t count);
  bool readPackedGridFile(const String& filePath, PackedQuarterGridV7* packed, size_t count);
  bool replaceFileWithPreparedTmp(fs::FS& fs, const String& tmpPath, const String& filePath);

  void packGridToV7(const std::vector<Tile>& grid, PackedQuarterGridV7* out_packed, uint16_t folder_id);
  void packTileToV7(const Tile& src, PackedTileV7& dst, uint16_t folder_id, size_t index);
  void unpackV7ToTile(const PackedTileV7& src, Tile& dst, uint16_t folder_id, size_t index);
};

}  // namespace uitiles

#endif  // UI_TILES_MODEL_H
