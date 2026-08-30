#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include "ui_fonts.h"
// Forward declaration of LVGL type — included only in .cpp files
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace web_admin_local {

void set_home_assistant_credentials(const std::string &url, const std::string &token);
bool toggle_home_assistant_entity(const char *entity_id, bool turn_on);

struct SwitchToggleContext {
  lv_obj_t *state_label;
  char entity_id[128];
};

// ── Home Assistant websocket live entity updates ─────────────────────────
//
// tile_widget_build_sensor() registers the value label / gauge arc of every
// TILE_SENSOR / TILE_ENERGY tile here, keyed by entity id. ha_ws_client.cpp
// looks entities up by id and calls apply_ha_entity_state() from the
// ESPHome loop() task whenever a matching `state_changed` event (or an
// initial get_states result) arrives over the websocket.

// Registers a sensor/energy tile's widgets for live updates. `gauge_arc`
// may be nullptr when the tile has no gauge. Safe to call only from code
// that owns the LVGL screen (tile building runs there already).
void register_ha_entity_widget(const std::string &entity_id, lv_obj_t *value_label,
                                lv_obj_t *gauge_arc, int decimals,
                                float gauge_min, float gauge_max);
void register_ha_switch_widget(const std::string &entity_id, lv_obj_t *switch_obj,
                               lv_obj_t *state_label);

// Forgets every registered widget binding. Must be called before a
// folder's tiles are torn down (lv_obj_clean) so a later websocket update
// can never dereference a stale LVGL object pointer.
void clear_ha_entity_widgets();

// Applies one Home Assistant entity state update to any currently
// registered widgets. MUST be called only from the ESPHome loop() task
// (see WebAdminLocal::loop() / ha_ws_client_loop()) -- never from the
// websocket client task.
void apply_ha_entity_state(const std::string &entity_id, const std::string &state,
                            const std::string &unit);

// Scans every stored folder tile grid (f0..f9) on the SD card and returns
// the sensor_entity / energy_entity ids currently configured, in no
// particular order and without duplicates. Used to build the Home
// Assistant websocket subscription filter (see ha_ws_client.h).
std::vector<std::string> collect_configured_ha_entities();

// ── Tile data ─────────────────────────────────────────────────────────────────

struct TileData {
  int     type        = 0;
  int     col         = 0;
  int     row         = 0;
  int     span_w      = 1;
  int     span_h      = 1;
  uint32_t bg_color   = 0;       // 0 = type default
  std::string icon_name;
  std::string title;
  std::string sensor_entity;
  std::string sensor_unit;
  int     sensor_decimals    = -1;
  int     sensor_display_mode = 0;
  float   sensor_gauge_min   = 0.f;
  float   sensor_gauge_max   = 100.f;
  std::string switch_entity;
  int     switch_style        = 0;
  std::string scene_alias;
  std::string weather_entity;
  std::string energy_entity;
  std::string media_entity;
  std::string climate_entity;
  std::string cover_entity;
  std::string camera_entity;
  std::string animation_file;
  std::string text_value;
  int     text_value_font     = 0;
  int     navigate_target     = 0;
  int     clock_flags         = 1;   // bit0=show_time, bit1=show_date
  int     clock_time_format   = 0;
  int     clock_date_format   = 0;
  int     key_code            = 40;  // clock time font
  int     key_modifier        = 20;  // clock date font
};

// Tile type constants matching admin.js / web tile types
static const int TILE_EMPTY    =  0;
static const int TILE_SENSOR   =  1;
static const int TILE_SCENE    =  2;
static const int TILE_NAVIGATE =  4;
static const int TILE_SWITCH   =  5;
static const int TILE_SETTINGS =  7;
static const int TILE_BACK     =  8;
static const int TILE_CLOCK    =  9;
static const int TILE_TEXT     = 10;
static const int TILE_WEATHER  = 12;
static const int TILE_ENERGY   = 14;
static const int TILE_MEDIA    = 15;
static const int TILE_ANIMATE  = 16;
// Stored configurations use both names; keep the spelling used by HomeTiles.
static const int TILE_PIXELANIM = TILE_ANIMATE;
static const int TILE_CLIMATE  = 17;
static const int TILE_CAMERA   = 18;
static const int TILE_COVER    = 19;

// ── Grid geometry (7 × 5, using the configured ESPHome LVGL screen) ──────────

struct GridGeometry {
  int screen_w  =  0;
  int screen_h  =  0;
  int cols      =    7;
  int rows      =    5;
  int pad       =    8;   // outer padding
  int gap       =    6;   // gap between cells

  int cell_w() const {
    return (screen_w - 2 * pad - (cols - 1) * gap) / cols;
  }
  int cell_h() const {
    return (screen_h - 2 * pad - (rows - 1) * gap) / rows;
  }
  int cell_x(int col) const { return pad + col * (cell_w() + gap); }
  int cell_y(int row) const { return pad + row * (cell_h() + gap); }
  int tile_w(int span_w) const { return span_w * cell_w() + (span_w - 1) * gap; }
  int tile_h(int span_h) const { return span_h * cell_h() + (span_h - 1) * gap; }
};

// ── Page info ─────────────────────────────────────────────────────────────────

struct FolderPage {
  int      folder_id  = 0;
  lv_obj_t *page      = nullptr;  // lv_obj screen object
};

// ── TilesLvglRenderer ─────────────────────────────────────────────────────────
//
// Renders tile-grid folders onto the active screen owned by ESPHome's LVGL
// component. Call refresh_folder(folder_id) after a grid JSON file changes to
// rebuild all tile widgets on that screen. Call show_folder(folder_id) to
// navigate.

class TilesLvglRenderer {
 public:
  TilesLvglRenderer() = default;

  // Must be called from setup() after ESPHome has initialized LVGL.
  void setup();

  // Rebuild the LVGL page for one folder.
  // Called from the POST handler after a successful tile save.
  void refresh_folder(int folder_id);

  // Request a rebuild from a non-LVGL task. The actual LVGL work is performed
  // by process_pending_refreshes() on the ESPHome loop task.
  void request_refresh_folder(int folder_id);
  void process_pending_refreshes();

  // Rebuild all known folder pages (e.g. on first boot).
  void refresh_all();

  // Navigate the display to a folder page.
  void show_folder(int folder_id);

  // Return the LVGL screen object for a folder (nullptr if not built yet).
  lv_obj_t *get_page(int folder_id) const;

 private:
  GridGeometry geo_;
  std::vector<FolderPage> pages_;
  bool occupied_[5][7] = {};
  std::atomic<uint16_t> pending_refresh_mask_{0};

  lv_obj_t *get_or_create_page(int folder_id);
  void build_folder_on_page(int folder_id, lv_obj_t *page,
                             const std::vector<TileData> &tiles);
  void build_tile(lv_obj_t *page, const TileData &tile);
};

// ── Global renderer instance (set in WebAdminLocal::setup) ───────────────────
// Declared here, defined in tiles_lvgl.cpp.
extern TilesLvglRenderer *g_tiles_renderer;

// Read tile grid for one folder from /sdcard/_tile_grids/fN.json.
std::vector<TileData> read_tile_grid_for_lvgl(int folder_id);

}  // namespace web_admin_local
