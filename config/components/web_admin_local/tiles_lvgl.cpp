#include "tiles_lvgl.h"
#include "web_admin_lvgl_fonts.h"
#include "mdi_icons.h"
#include "ha_ws_client.h"
#include <esp_log.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unordered_map>

static const char *TAG = "tiles_lvgl";

namespace web_admin_local {

TilesLvglRenderer *g_tiles_renderer = nullptr;
static std::string home_assistant_url;
static std::string home_assistant_token;

void set_home_assistant_credentials(const std::string &url, const std::string &token) {
  home_assistant_url = url;
  home_assistant_token = token;
}

// ── Home Assistant websocket live entity updates ─────────────────────────
// See tiles_lvgl.h for the public API. Widgets are registered while a
// folder page is built (tile_widget_build_sensor) and forgotten right
// before that page is torn down (build_folder_on_page), so a stale
// websocket update can never touch a destroyed LVGL object.

namespace {

struct SensorWidgetBinding {
  lv_obj_t *value_label = nullptr;
  lv_obj_t *gauge_arc = nullptr;
  lv_obj_t *switch_obj = nullptr;
  lv_obj_t *state_label = nullptr;
  int decimals = -1;
  float gauge_min = 0.f;
  float gauge_max = 100.f;
};

std::unordered_map<std::string, std::vector<SensorWidgetBinding>> g_sensor_widget_bindings;

SemaphoreHandle_t widget_registry_mutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

struct MutexGuard {
  explicit MutexGuard(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGive(mutex_); }
  SemaphoreHandle_t mutex_;
};

}  // namespace

void register_ha_entity_widget(const std::string &entity_id, lv_obj_t *value_label,
                                lv_obj_t *gauge_arc, int decimals,
                                float gauge_min, float gauge_max) {
  if (entity_id.empty()) return;
  SensorWidgetBinding binding;
  binding.value_label = value_label;
  binding.gauge_arc = gauge_arc;
  binding.decimals = decimals;
  binding.gauge_min = gauge_min;
  binding.gauge_max = gauge_max;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
}

void register_ha_switch_widget(const std::string &entity_id, lv_obj_t *switch_obj,
                               lv_obj_t *state_label) {
  if (entity_id.empty() || switch_obj == nullptr) return;
  SensorWidgetBinding binding;
  binding.switch_obj = switch_obj;
  binding.state_label = state_label;
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings[entity_id].push_back(binding);
}

void clear_ha_entity_widgets() {
  MutexGuard lock(widget_registry_mutex());
  g_sensor_widget_bindings.clear();
}

void apply_ha_entity_state(const std::string &entity_id, const std::string &state,
                            const std::string &unit) {
  (void) unit;  // the unit label is fixed at tile-build time; only the value/gauge live-update.
  if (entity_id.empty()) return;

  std::vector<SensorWidgetBinding> bindings;
  {
    MutexGuard lock(widget_registry_mutex());
    const auto it = g_sensor_widget_bindings.find(entity_id);
    if (it == g_sensor_widget_bindings.end()) return;
    bindings = it->second;  // copy out; LVGL calls below happen unlocked
  }

  char *num_end = nullptr;
  const double numeric_value = strtod(state.c_str(), &num_end);
  const bool is_numeric = num_end != state.c_str() && *num_end == '\0';

  for (const auto &binding : bindings) {
    if (binding.switch_obj != nullptr) {
      std::string normalized = state;
      std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (normalized == "on" || normalized == "off") {
        const bool is_on = normalized == "on";
        if (is_on) lv_obj_add_state(binding.switch_obj, LV_STATE_CHECKED);
        else lv_obj_clear_state(binding.switch_obj, LV_STATE_CHECKED);
        lv_obj_t *caption = lv_obj_get_child(binding.switch_obj, 0);
        if (caption) lv_label_set_text(caption, is_on ? "ON" : "OFF");
        if (binding.state_label) lv_label_set_text(binding.state_label, is_on ? "ON" : "OFF");
      }
      continue;
    }
    if (binding.value_label != nullptr) {
      char buf[40];
      if (is_numeric && binding.decimals >= 0) {
        snprintf(buf, sizeof(buf), "%.*f", binding.decimals, numeric_value);
      } else {
        snprintf(buf, sizeof(buf), "%s", state.c_str());
      }
      lv_label_set_text(binding.value_label, buf);
    }
    if (binding.gauge_arc != nullptr && is_numeric) {
      const float clamped = std::max(binding.gauge_min, std::min(binding.gauge_max, static_cast<float>(numeric_value)));
      lv_arc_set_value(binding.gauge_arc, static_cast<int>(clamped));
    }
  }
}

std::vector<std::string> collect_configured_ha_entities() {
  std::vector<std::string> entities;
  auto add_unique = [&](const std::string &id) {
    if (id.empty()) return;
    for (const auto &existing : entities) {
      if (existing == id) return;
    }
    entities.push_back(id);
  };
  for (int folder_id = 0; folder_id <= 9; folder_id++) {
    char path[80];
    snprintf(path, sizeof(path), "/sdcard/_tile_grids/f%d.json", folder_id);
    FILE *probe = fopen(path, "rb");
    if (!probe) continue;
    fclose(probe);
    for (const auto &tile : read_tile_grid_for_lvgl(folder_id)) {
      if (tile.type == TILE_SENSOR || tile.type == TILE_ENERGY) {
        add_unique(tile.sensor_entity);
        add_unique(tile.energy_entity);
      } else if (tile.type == TILE_SWITCH) {
        add_unique(tile.switch_entity.empty() ? tile.sensor_entity : tile.switch_entity);
      }
    }
  }
  return entities;
}

bool toggle_home_assistant_entity(const char *entity_id, bool turn_on) {
  if (entity_id == nullptr || entity_id[0] == '\0' ||
      home_assistant_url.empty() || home_assistant_token.empty()) {
    ESP_LOGW(TAG, "Cannot toggle entity: Home Assistant REST API is not configured");
    return false;
  }

  const char *dot = strchr(entity_id, '.');
  if (dot == nullptr || dot == entity_id) {
    ESP_LOGW(TAG, "Cannot toggle invalid entity ID: %s", entity_id);
    return false;
  }
  const std::string domain(entity_id, static_cast<size_t>(dot - entity_id));
  if (domain != "switch" && domain != "light" && domain != "input_boolean") {
    ESP_LOGW(TAG, "Cannot toggle unsupported entity: %s", entity_id);
    return false;
  }

  std::string url = home_assistant_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/services/" + domain + (turn_on ? "/turn_on" : "/turn_off");

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Unable to initialize Home Assistant REST client");
    return false;
  }

  const std::string auth = "Bearer " + home_assistant_token;
  const std::string body = std::string("{\"entity_id\":\"") + entity_id + "\"}";
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Home Assistant toggle failed for %s (status=%d, error=%s)",
             entity_id, status, esp_err_to_name(result));
    return false;
  }
  return true;
}

// ── JSON reader ───────────────────────────────────────────────────────────────

std::vector<TileData> read_tile_grid_for_lvgl(int folder_id) {
  std::vector<TileData> result(35);
  // Fill default grid positions
  for (int i = 0; i < 35; i++) {
    result[i].col = i % 7;
    result[i].row = i / 7;
  }

  char path[80];
  snprintf(path, sizeof(path), "/sdcard/_tile_grids/f%d.json", folder_id);
  FILE *f = fopen(path, "rb");
  if (!f) return result;

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz <= 0 || sz > 65536) { fclose(f); return result; }

  std::string raw(static_cast<size_t>(sz), '\0');
  fread(&raw[0], 1, static_cast<size_t>(sz), f);
  fclose(f);

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return result;

  JsonArray arr;
  if (doc.is<JsonArray>())                arr = doc.as<JsonArray>();
  else if (doc["tiles"].is<JsonArray>())  arr = doc["tiles"].as<JsonArray>();
  else return result;

  int idx = 0;
  for (JsonObject t : arr) {
    if (idx >= 35) break;
    TileData &d = result[idx];
    d.type              = t["type"]             | 0;
    d.col               = t["col"]              | (idx % 7);
    d.row               = t["row"]              | (idx / 7);
    d.span_w            = std::max(1, (int)(t["span_w"] | 1));
    d.span_h            = std::max(1, (int)(t["span_h"] | 1));
    d.bg_color          = t["bg_color"]         | 0u;
    d.icon_name         = t["icon_name"]        | "";
    d.title             = t["title"]            | "";
    d.sensor_entity     = t["sensor_entity"]    | "";
    d.sensor_unit       = t["sensor_unit"]      | "";
    d.sensor_decimals   = t["sensor_decimals"]  | -1;
    d.sensor_display_mode = t["sensor_display_mode"] | 0;
    d.sensor_gauge_min  = t["sensor_gauge_min"] | 0.0f;
    d.sensor_gauge_max  = t["sensor_gauge_max"] | 100.0f;
    d.switch_entity     = t["switch_entity"]    | "";
    d.switch_style      = t["switch_style"]     | 0;
    d.scene_alias       = t["scene_alias"]      | "";
    d.weather_entity    = t["weather_entity"]   | "";
    d.energy_entity     = t["energy_entity"]    | "";
    d.media_entity      = t["media_entity"]     | "";
    d.climate_entity    = t["climate_entity"]   | "";
    d.cover_entity      = t["cover_entity"]     | "";
    d.camera_entity     = t["camera_entity"]    | "";
    d.animation_file    = t["animation_file"]   | "";
    d.text_value        = t["text_value"]       | "";
    d.text_value_font   = t["text_value_font"]  | 0;
    d.navigate_target   = t["navigate_target"]  | 0;
    d.clock_flags       = t["clock_flags"]      | 1;
    d.clock_time_format = t["clock_time_format"]| 0;
    d.clock_date_format = t["clock_date_format"]| 0;
    d.key_code          = t["key_code"]         | 40;
    d.key_modifier      = t["key_modifier"]     | 20;
    idx++;
  }
  return result;
}

// ── Forward declarations from per-type files ──────────────────────────────────

void tile_widget_build_sensor(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_clock(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_navigate(lv_obj_t *parent, const TileData &tile, int folder_id);
void tile_widget_build_scene(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_energy(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_media(lv_obj_t *parent, const TileData &tile);
void tile_widget_build_text(lv_obj_t *parent, const TileData &tile);

// ── Shared helper: create a label with given text, colour, font size ──────────

lv_obj_t *lvgl_tile_make_label(lv_obj_t *parent, const char *text,
                                lv_color_t color, int font_size,
                                lv_align_t align = LV_ALIGN_CENTER) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text ? text : "");
  lv_obj_set_style_text_color(lbl, color, 0);
  lv_obj_set_style_text_font(lbl, web_admin_lvgl_font_for_size(font_size), 0);
  lv_obj_align(lbl, align, 0, 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(90));
  return lbl;
}

// ── Resolve tile background colour ────────────────────────────────────────────

static lv_color_t tile_bg_color(const TileData &tile) {
  uint32_t rgb = tile.bg_color & 0xFFFFFF;
  if (rgb == 0) {
    // Per-type defaults (matching admin.css preview defaults)
    switch (tile.type) {
      case TILE_SENSOR:   rgb = 0x1E3A5F; break;
      case TILE_SWITCH:   rgb = 0x2E4A1E; break;
      case TILE_SCENE:    rgb = 0x3A2E1E; break;
      case TILE_CLOCK:    rgb = 0x1A1A2E; break;
      case TILE_WEATHER:  rgb = 0x1A3A4A; break;
      case TILE_ENERGY:   rgb = 0x3A2A0A; break;
      case TILE_MEDIA:    rgb = 0x2A1A3A; break;
      case TILE_NAVIGATE: rgb = 0x1A2A3A; break;
      case TILE_CLIMATE:  rgb = 0x2A1A1A; break;
      default:            rgb = 0x353535; break;
    }

  }
  return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static std::string tile_icon_name(const TileData &tile) {
  if (isMdiIconDisabled(tile.icon_name)) return {};
  std::string icon = normalizeMdiIconName(tile.icon_name);
  if (!icon.empty()) return icon;
  switch (tile.type) {
    case TILE_SENSOR:   return "thermometer";
    case TILE_SWITCH:   return "lightbulb";
    case TILE_SCENE:    return "play-circle";
    case TILE_NAVIGATE: return "folder";
    case TILE_CLOCK:    return "clock-outline";
    case TILE_WEATHER:  return "weather-partly-cloudy";
    case TILE_ENERGY:   return "flash";
    case TILE_MEDIA:    return "television";
    case TILE_CLIMATE:  return "thermostat";
    case TILE_CAMERA:   return "video";
    case TILE_COVER:    return "window-shutter";
    case TILE_SETTINGS: return "cog";
    case TILE_BACK:     return "arrow-left";
    case TILE_ANIMATE:  return "animation-outline";
    default:            return {};
  }
}

// ── Build one tile object ─────────────────────────────────────────────────────

static void tile_click_cb(lv_event_t *e) {
  // Navigate to a folder when a NAVIGATE tile is clicked.
  // We store the target folder_id in user_data.
  int target = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_tiles_renderer && target >= 0) {
    g_tiles_renderer->show_folder(target);
  }
}

void TilesLvglRenderer::build_tile(lv_obj_t *page, const TileData &tile) {
  if (tile.type == TILE_EMPTY) return;

  // Clamp layout
  int col    = std::max(0, std::min(tile.col,    geo_.cols - 1));
  int row    = std::max(0, std::min(tile.row,    geo_.rows - 1));
  int span_w = std::max(1, std::min(tile.span_w, geo_.cols - col));
  int span_h = std::max(1, std::min(tile.span_h, geo_.rows - row));
  for (int r = row; r < row + span_h; ++r)
    for (int c = col; c < col + span_w; ++c)
      if (occupied_[r][c]) return;
  for (int r = row; r < row + span_h; ++r)
    for (int c = col; c < col + span_w; ++c)
      occupied_[r][c] = true;

  int x = geo_.cell_x(col);
  int y = geo_.cell_y(row);
  int w = geo_.tile_w(span_w);
  int h = geo_.tile_h(span_h);

  // Outer tile container
  lv_obj_t *tile_obj = lv_button_create(page);
  lv_obj_set_pos(tile_obj, x, y);
  lv_obj_set_size(tile_obj, w, h);
  lv_obj_set_style_radius(tile_obj, 12, 0);
  lv_obj_set_style_bg_color(tile_obj, tile_bg_color(tile), 0);
  const lv_color32_t base = lv_color_to_32(tile_bg_color(tile), LV_OPA_COVER);
  const lv_color_t pressed = lv_color_make(
      static_cast<uint8_t>(std::min<int>(255, base.red + 16)),
      static_cast<uint8_t>(std::min<int>(255, base.green + 16)),
      static_cast<uint8_t>(std::min<int>(255, base.blue + 16)));
  lv_obj_set_style_bg_color(tile_obj, pressed, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(tile_obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(tile_obj, 0, 0);
  lv_obj_set_style_shadow_width(tile_obj, 0, 0);
  lv_obj_set_style_pad_all(tile_obj, 8, 0);
  lv_obj_set_style_clip_corner(tile_obj, true, 0);
  lv_obj_clear_flag(tile_obj, LV_OBJ_FLAG_SCROLLABLE);

  // Make navigate tiles clickable
  if (tile.type == TILE_NAVIGATE && tile.navigate_target >= 0) {
    lv_obj_add_flag(tile_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile_obj, tile_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)tile.navigate_target);
  }

  const std::string configured_icon = tile_icon_name(tile);
  if (!configured_icon.empty()) {
    const std::string icon_char = getMdiChar(configured_icon);
    if (!icon_char.empty()) {
      lv_obj_t *icon = lv_label_create(tile_obj);
      lv_label_set_text(icon, icon_char.c_str());
      lv_obj_set_style_text_color(icon, lv_color_white(), 0);
      lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
      lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
  }

  // Delegate to per-type widget builders
  switch (tile.type) {
    case TILE_SENSOR:
    case TILE_ENERGY:   tile_widget_build_sensor(tile_obj, tile); break;
    case TILE_CLOCK:    tile_widget_build_clock(tile_obj, tile);  break;
    case TILE_SWITCH:   tile_widget_build_switch(tile_obj, tile); break;
    case TILE_NAVIGATE: tile_widget_build_navigate(tile_obj, tile, tile.navigate_target); break;
    case TILE_SCENE:    tile_widget_build_scene(tile_obj, tile);  break;
    case TILE_WEATHER:  tile_widget_build_weather(tile_obj, tile); break;
    case TILE_MEDIA:    tile_widget_build_media(tile_obj, tile);  break;
    case TILE_TEXT:     tile_widget_build_text(tile_obj, tile);   break;
    case TILE_CLIMATE:
    case TILE_CAMERA:
    case TILE_COVER:
    case TILE_ANIMATE:
      {
        const std::string icon = configured_icon;
        const std::string label = tile.title.empty()
          ? (tile.type == TILE_CLIMATE ? "Climate" :
             tile.type == TILE_CAMERA ? "Camera" :
             tile.type == TILE_COVER ? "Cover" : "Animation")
          : tile.title;
        lv_obj_t *value = lv_label_create(tile_obj);
        lv_label_set_text(value, label.c_str());
        lv_obj_set_style_text_color(value, lv_color_white(), 0);
        lv_obj_set_style_text_font(value, web_admin_lvgl_font_for_size(20), 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(value, LV_PCT(100));
        lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, 0);
        (void)icon;
      }
      break;
    case TILE_SETTINGS:
    case TILE_BACK:
      {
        lv_obj_t *lbl = lv_label_create(tile_obj);
        const char *fallback = tile.type == TILE_BACK ? "Back" : "Settings";
        lv_label_set_text(lbl, tile.title.empty() ? fallback : tile.title.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, web_admin_lvgl_font_for_size(20), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        if (tile.type == TILE_BACK) {
          lv_obj_add_flag(tile_obj, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_add_event_cb(tile_obj, tile_click_cb, LV_EVENT_CLICKED,
                              (void *)(intptr_t)0);
        }
      }
      break;
    default:
      // Unknown type: show type number as text
      {
        char buf[32]; snprintf(buf, sizeof(buf), "Type %d", tile.type);
        lv_obj_t *lbl = lv_label_create(tile_obj);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_make(180,180,180), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
      }
      break;
  }
}

// ── Build a complete folder page ──────────────────────────────────────────────

void TilesLvglRenderer::build_folder_on_page(int folder_id, lv_obj_t *page,
                                              const std::vector<TileData> &tiles) {
  // Forget any Home Assistant websocket widget bindings for the tiles this
  // page currently holds *before* destroying them, so a state update that
  // arrives mid-rebuild can never touch a dangling LVGL object pointer.
  clear_ha_entity_widgets();

  // Remove all existing children
  lv_obj_clean(page);

  // Dark background
  lv_obj_set_style_bg_color(page, lv_color_make(0x0A, 0x0A, 0x0A), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  memset(occupied_, 0, sizeof(occupied_));

  for (const auto &tile : tiles) {
    build_tile(page, tile);
  }

  ESP_LOGI(TAG, "Built folder %d with %zu tiles", folder_id,
           std::count_if(tiles.begin(), tiles.end(),
                         [](const TileData &t){ return t.type != TILE_EMPTY; }));
}

// ── Page management ───────────────────────────────────────────────────────────

lv_obj_t *TilesLvglRenderer::get_or_create_page(int folder_id) {
  for (auto &p : pages_) {
    if (p.folder_id == folder_id) return p.page;
  }
  // Do not create a separate LVGL screen here. ESPHome owns the configured
  // screen/page hierarchy; tile folders are rendered on its active screen.
  lv_obj_t *screen = lv_scr_act();
  pages_.push_back({folder_id, screen});
  ESP_LOGI(TAG, "Using ESPHome LVGL screen for folder %d", folder_id);
  return screen;
}

lv_obj_t *TilesLvglRenderer::get_page(int folder_id) const {
  for (const auto &p : pages_) {
    if (p.folder_id == folder_id) return p.page;
  }
  return nullptr;
}

// ── Public interface ──────────────────────────────────────────────────────────

void TilesLvglRenderer::setup() {
  lv_obj_t *screen = lv_scr_act();
  geo_.screen_w = lv_obj_get_width(screen);
  geo_.screen_h = lv_obj_get_height(screen);
  ESP_LOGI(TAG, "TilesLvglRenderer setup: %dx%d grid %dx%d cell %dx%d",
           geo_.screen_w, geo_.screen_h, geo_.cols, geo_.rows,
           geo_.cell_w(), geo_.cell_h());
}

void TilesLvglRenderer::refresh_folder(int folder_id) {
  if (folder_id < 0 || folder_id > 9) return;
  const auto tiles = read_tile_grid_for_lvgl(folder_id);
  lv_obj_t *page = get_or_create_page(folder_id);
  build_folder_on_page(folder_id, page, tiles);
  // Keep the websocket subscription filter in sync with whatever is
  // currently configured across all folders (cheap: a handful of small
  // SD-card JSON files).
  ha_ws_client_set_entity_filter(collect_configured_ha_entities());
}

void TilesLvglRenderer::request_refresh_folder(int folder_id) {
  if (folder_id < 0 || folder_id > 9) return;
  pending_refresh_mask_.fetch_or(static_cast<uint16_t>(1U << folder_id),
                                 std::memory_order_relaxed);
}

void TilesLvglRenderer::process_pending_refreshes() {
  const uint16_t pending = pending_refresh_mask_.exchange(0, std::memory_order_relaxed);
  for (int folder_id = 0; folder_id <= 9; ++folder_id) {
    if ((pending & static_cast<uint16_t>(1U << folder_id)) != 0) {
      refresh_folder(folder_id);
    }
  }
}

void TilesLvglRenderer::refresh_all() {
  for (int i = 0; i <= 9; i++) {
    char path[80];
    snprintf(path, sizeof(path), "/sdcard/_tile_grids/f%d.json", i);
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    fclose(f);
    refresh_folder(i);
  }
}

void TilesLvglRenderer::show_folder(int folder_id) {
  lv_obj_t *page = get_page(folder_id);
  if (!page) {
    refresh_folder(folder_id);
    page = get_page(folder_id);
  }
  if (page) {
    // The page pointer is the active ESPHome LVGL screen. Rebuild it instead
    // of loading a screen that was not created by the ESPHome component.
    refresh_folder(folder_id);
    ESP_LOGI(TAG, "Showing folder %d", folder_id);
  }
}

}  // namespace web_admin_local

// ── Tile type widget implementations (inlined from tile_types/) ────────────
// ─── tile_widget_sensor.cpp ───
// Sensor / Energy tile: shows entity value, unit, optional gauge.

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
  lv_obj_set_style_text_font(title_lbl, web_admin_lvgl_font_for_size(16), 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  // Value label in the center (placeholder — live value updated by HA bridge)
  char val_buf[32];
  snprintf(val_buf, sizeof(val_buf), "--");
  lv_obj_t *val_lbl = lv_label_create(parent);
  lv_label_set_text(val_lbl, val_buf);
  lv_obj_set_style_text_color(val_lbl, white, 0);
  lv_obj_set_style_text_font(val_lbl, web_admin_lvgl_font_for_size(28), 0);
  lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, 0);

  // Unit label below value
  if (!tile.sensor_unit.empty()) {
    lv_obj_t *unit_lbl = lv_label_create(parent);
    lv_label_set_text(unit_lbl, tile.sensor_unit.c_str());
    lv_obj_set_style_text_color(unit_lbl, muted, 0);
    lv_obj_set_style_text_font(unit_lbl, web_admin_lvgl_font_for_size(16), 0);
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

// ─── tile_widget_clock.cpp ───
// Clock tile: shows HH:MM time and optional date.

namespace web_admin_local {


void tile_widget_build_clock(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);
  bool show_time = (tile.clock_flags & 1) != 0;
  bool show_date = (tile.clock_flags & 2) != 0;

  // Title (optional)
  if (!tile.title.empty()) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, tile.title.c_str());
    lv_obj_set_style_text_color(lbl, muted, 0);
    lv_obj_set_style_text_font(lbl, web_admin_lvgl_font_for_size(16), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Time HH:MM
  if (show_time) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char tbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M", &tm_info);

    lv_obj_t *time_lbl = lv_label_create(parent);
    lv_label_set_text(time_lbl, tbuf);
    lv_obj_set_style_text_color(time_lbl, white, 0);
    // Time font size from key_code (stored as clock time font size, default 40)
    const lv_font_t *font = web_admin_lvgl_font_for_size(tile.key_code);
    lv_obj_set_style_text_font(time_lbl, font, 0);
    int y_offset = show_date ? -12 : 0;
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, y_offset);

    // Register a 1-second timer to keep the display updated
    lv_timer_create([](lv_timer_t *timer) {
      lv_obj_t *lbl = (lv_obj_t *)lv_timer_get_user_data(timer);
      if (!lbl || !lv_obj_is_valid(lbl)) {
        lv_timer_delete(timer);
        return;
      }
      time_t n = time(nullptr);
      struct tm tm;
      localtime_r(&n, &tm);
      char b[16];
      strftime(b, sizeof(b), "%H:%M", &tm);
      lv_label_set_text(lbl, b);
    }, 1000, time_lbl);
  }

  // Date
  if (show_date) {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char dbuf[32];
    // Format based on clock_date_format: 0=auto, 1=DD.MM.YYYY, 2=MM/DD/YYYY
    const char *fmt = (tile.clock_date_format == 2) ? "%m/%d/%Y"
                    : (tile.clock_date_format == 3) ? "%Y/%m/%d"
                    : "%d.%m.%Y";
    strftime(dbuf, sizeof(dbuf), fmt, &tm_info);

    lv_obj_t *date_lbl = lv_label_create(parent);
    lv_label_set_text(date_lbl, dbuf);
    lv_obj_set_style_text_color(date_lbl, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_text_font(date_lbl, web_admin_lvgl_font_for_size(20), 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, show_time ? 22 : 0);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_switch.cpp ───
// Switch / Light tile: toggle button or brightness slider.

namespace web_admin_local {

static void switch_toggle_cb(lv_event_t *e) {
  auto *context = static_cast<SwitchToggleContext *>(lv_event_get_user_data(e));
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  if (!sw || context == nullptr) return;
  const bool turn_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (!toggle_home_assistant_entity(context->entity_id, turn_on)) {
    if (turn_on) lv_obj_clear_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  const char *state = lv_obj_has_state(sw, LV_STATE_CHECKED) ? "ON" : "OFF";
  // Keep both the control caption and the footer state in sync.  The state
  // label is passed as user data because the button owns its caption.
  lv_obj_t *caption = lv_obj_get_child(sw, 0);
  if (caption) lv_label_set_text(caption, state);
  if (context->state_label) lv_label_set_text(context->state_label, state);
}

void tile_widget_build_switch(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);
  const std::string &entity = tile.switch_entity.empty()
                              ? tile.sensor_entity : tile.switch_entity;

  // Title label
  const char *heading = tile.title.empty() ? entity.c_str() : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, heading);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, muted, 0);
  lv_obj_set_style_text_font(title, web_admin_lvgl_font_for_size(16), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  // Toggle button.  Using a checkable button keeps this compatible with the
  // small LVGL feature set used by ESPHome builds.
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, LV_PCT(70), 36);
  lv_obj_set_style_bg_color(btn, lv_color_make(0x2A, 0x2A, 0x2A), 0);
  lv_obj_set_style_bg_color(btn, lv_color_make(0x3B, 0x82, 0xF6), LV_STATE_CHECKED);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_t *btn_lbl = lv_label_create(btn);
  lv_label_set_text(btn_lbl, "OFF");
  lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
  lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);
  // State label at bottom
  lv_obj_t *state_lbl = lv_label_create(parent);
  lv_label_set_text(state_lbl, "OFF");
  lv_obj_set_style_text_color(state_lbl, muted, 0);
  lv_obj_set_style_text_font(state_lbl, web_admin_lvgl_font_for_size(16), 0);
  lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  auto *context = new SwitchToggleContext{state_lbl, {}};
  std::strncpy(context->entity_id, entity.c_str(), sizeof(context->entity_id) - 1);
  context->entity_id[sizeof(context->entity_id) - 1] = '\0';
  lv_obj_add_event_cb(btn, switch_toggle_cb, LV_EVENT_VALUE_CHANGED, context);
  if (!entity.empty()) register_ha_switch_widget(entity, btn, state_lbl);
}

}  // namespace web_admin_local

// ─── tile_widget_navigate.cpp ───
// Navigate tile: tapping navigates to another folder page.

namespace web_admin_local {

void tile_widget_build_navigate(lv_obj_t *parent, const TileData &tile, int /*folder_id*/) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Arrow icon at centre-top
  lv_obj_t *arrow = lv_label_create(parent);
  lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(arrow, lv_color_make(0x26, 0xA6, 0x9A), 0);
  lv_obj_set_style_text_font(arrow, web_admin_lvgl_font_for_size(24), 0);
  lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, 0, 0);

  // Folder name
  const char *name = tile.title.empty() ? "Folder" : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, web_admin_lvgl_font_for_size(20), 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

  // "Tap to enter" hint
  lv_obj_t *hint = lv_label_create(parent);
  lv_label_set_text(hint, "Tap to open");
  lv_obj_set_style_text_color(hint, muted, 0);
  lv_obj_set_style_text_font(hint, web_admin_lvgl_font_for_size(16), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

}  // namespace web_admin_local

// ─── tile_widget_scene.cpp ───
// Scene / Script tile: tap-to-trigger action tile.

namespace web_admin_local {

void tile_widget_build_scene(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);

  // Large play icon
  lv_obj_t *icon = lv_label_create(parent);
  lv_label_set_text(icon, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(icon, accent, 0);
  lv_obj_set_style_text_font(icon, web_admin_lvgl_font_for_size(28), 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);

  // Scene / script name
  const char *name = tile.title.empty() ? tile.scene_alias.c_str() : tile.title.c_str();
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, web_admin_lvgl_font_for_size(16), 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
}

}  // namespace web_admin_local

// ─── tile_widget_weather.cpp ───
// Weather tile: shows condition + temperature.

namespace web_admin_local {

void tile_widget_build_weather(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Title
  if (!tile.title.empty()) {
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, tile.title.c_str());
    lv_obj_set_style_text_color(t, muted, 0);
    lv_obj_set_style_text_font(t, web_admin_lvgl_font_for_size(16), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Resolve the configured MDI icon first, falling back to a useful weather
  // icon.  LV_SYMBOL_WARNING is not part of the MDI font and renders
  // inconsistently across LVGL font configurations.
  lv_obj_t *icon = lv_label_create(parent);
  const std::string icon_name = tile_icon_name(tile);
  const std::string icon_char = getMdiChar(
      icon_name.empty() ? "weather-partly-cloudy" : icon_name);
  lv_label_set_text(icon, icon_char.empty() ? "?" : icon_char.c_str());
  lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0xD5, 0x4F), 0);
  lv_obj_set_style_text_font(icon, FONT_MDI_ICONS, 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, -20, 0);

  // Temperature placeholder
  lv_obj_t *temp = lv_label_create(parent);
  lv_label_set_text(temp, "--°");
  lv_obj_set_style_text_color(temp, white, 0);
  lv_obj_set_style_text_font(temp, web_admin_lvgl_font_for_size(28), 0);
  lv_obj_align(temp, LV_ALIGN_CENTER, 28, 0);

  // Entity hint
  const std::string &entity = tile.weather_entity.empty()
                              ? tile.sensor_entity : tile.weather_entity;
  if (!entity.empty()) {
    lv_obj_t *e = lv_label_create(parent);
    lv_label_set_text(e, entity.c_str());
    lv_label_set_long_mode(e, LV_LABEL_LONG_DOT);
    lv_obj_set_width(e, LV_PCT(100));
    lv_obj_set_style_text_color(e, muted, 0);
    lv_obj_set_style_text_font(e, web_admin_lvgl_font_for_size(16), 0);
    lv_obj_align(e, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_media.cpp ───
// Media player tile: shows track info + playback controls.

namespace web_admin_local {

void tile_widget_build_media(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white  = lv_color_white();
  const lv_color_t muted  = lv_color_make(0x8A, 0x8A, 0x8A);
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);

  // Title / player name at top
  const char *name = tile.title.empty()
                   ? (tile.media_entity.empty() ? "Media" : tile.media_entity.c_str())
                   : tile.title.c_str();
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, name);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_color(title, muted, 0);
  lv_obj_set_style_text_font(title, web_admin_lvgl_font_for_size(16), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  // "Now playing" placeholder
  lv_obj_t *track = lv_label_create(parent);
  lv_label_set_text(track, "--");
  lv_obj_set_style_text_color(track, white, 0);
  lv_obj_set_style_text_font(track, web_admin_lvgl_font_for_size(16), 0);
  lv_label_set_long_mode(track, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(track, LV_PCT(100));
  lv_obj_align(track, LV_ALIGN_CENTER, 0, -8);

  // Playback buttons row
  lv_obj_t *btn_row = lv_obj_create(parent);
  lv_obj_set_size(btn_row, LV_PCT(100), 36);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_set_style_pad_all(btn_row, 0, 0);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

  const char *btns[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
  for (auto *sym : btns) {
    lv_obj_t *btn = lv_button_create(btn_row);
    lv_obj_set_size(btn, 36, 32);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_bg_color(btn, accent, LV_STATE_PRESSED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, white, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  }
}

}  // namespace web_admin_local

// ─── tile_widget_text.cpp ───
// Text tile: displays a static text value.

namespace web_admin_local {

void tile_widget_build_text(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t muted = lv_color_make(0x8A, 0x8A, 0x8A);

  // Optional small title at top
  if (!tile.title.empty()) {
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, tile.title.c_str());
    lv_obj_set_style_text_color(t, muted, 0);
    lv_obj_set_style_text_font(t, web_admin_lvgl_font_for_size(16), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Choose font based on text_value_font index (0=default 28, 1=36, etc.)
  const lv_font_t *font;
  switch (tile.text_value_font) {
    case 1:  font = web_admin_lvgl_font_semibold(); break;
    case 2:  font = web_admin_lvgl_font_for_size(28); break;
    default: font = web_admin_lvgl_font_for_size(20); break;
  }

  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, tile.text_value.empty() ? "--" : tile.text_value.c_str());
  lv_obj_set_style_text_color(lbl, white, 0);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, tile.title.empty() ? 0 : 8);
}

}  // namespace web_admin_local

// ─── tile_widget_energy.cpp ───
// Energy / power tile — re-uses sensor layout.
// This file is intentionally minimal: build_sensor already handles TILE_ENERGY
// because the TilesLvglRenderer dispatches both TILE_SENSOR and TILE_ENERGY to
// tile_widget_build_sensor().  This file provides the stub forward declaration
// so that the linker is satisfied.

// tile_widget_build_sensor declared in tile_widget_sensor.cpp
// No separate implementation needed for energy — handled by shared sensor widget.
