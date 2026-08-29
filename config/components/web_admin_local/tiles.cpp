#include "tiles.h"
#include "tiles_lvgl.h"
#include <esp_log.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include <esp_http_client.h>

// Forward-declare asset accessors defined in web_admin_assets.cpp
const char* adminJsAssetPath();
const char* adminCssAssetPath();

namespace web_admin_local {

static std::string request_url(AsyncWebServerRequest *request) {
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  return request->url_to(url_buffer).str();
}

// ── Folder metadata helpers ──────────────────────────────────────────────────

struct FolderMeta {
  int    id        = 0;
  int    parent_id = 0;
  std::string name;
  std::string icon;
};

// Read folder list from /sdcard/_tile_grids/folders.json.
// Falls back to synthetic entries if the file is absent.
static std::vector<FolderMeta> readFolderMetaList() {
  std::vector<FolderMeta> list;
  FILE *f = fopen("/sdcard/_tile_grids/folders.json", "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz > 0 && sz < 8192) {
      std::string raw(static_cast<size_t>(sz), '\0');
      if (fread(&raw[0], 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz)) {
        JsonDocument doc;
        if (!deserializeJson(doc, raw) && doc["folders"].is<JsonArray>()) {
          for (JsonObject entry : doc["folders"].as<JsonArray>()) {
            FolderMeta m;
            m.id        = entry["id"]        | 0;
            m.parent_id = entry["parent_id"] | 0;
            m.name      = entry["name"]      | "";
            m.icon      = entry["icon_name"] | "";
            list.push_back(m);
          }
        }
      }
    }
    fclose(f);
  }
  // If JSON gave us nothing fall back to scanning for grid files
  if (list.empty()) {
    for (int i = 0; i <= 9; i++) {
      char p[80];
      snprintf(p, sizeof(p), "/sdcard/_tile_grids/f%d.json", i);
      FILE *gf = fopen(p, "rb");
      if (!gf) continue;
      fclose(gf);
      FolderMeta m;
      m.id   = i;
      m.name = (i == 0) ? "Home" : ("Folder " + std::to_string(i + 1));
      list.push_back(m);
    }
  }
  if (list.empty()) {
    FolderMeta m; m.id = 0; m.name = "Home";
    list.push_back(m);
  }
  return list;
}

static FolderMeta getFolderMeta(int folder_id) {
  auto list = readFolderMetaList();
  for (auto& m : list) { if (m.id == folder_id) return m; }
  FolderMeta m;
  m.id   = folder_id;
  m.name = (folder_id == 0) ? "Home" : ("Folder " + std::to_string(folder_id + 1));
  return m;
}

// ── HTML helpers ─────────────────────────────────────────────────────────────

static std::string htmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      default:   out += c;        break;
    }
  }
  return out;
}

// Build the tab-nav button for a folder.
static std::string buildFolderButtonHtml(const FolderMeta& m) {
  const std::string tid = "folder" + std::to_string(m.id);
  const std::string label = m.name.empty()
    ? (m.id == 0 ? "Home" : "Folder " + std::to_string(m.id + 1))
    : m.name;
  std::string icon = m.icon;
  // Strip mdi: / mdi- prefix if present
  if (icon.rfind("mdi:", 0) == 0) icon = icon.substr(4);
  else if (icon.rfind("mdi-", 0) == 0) icon = icon.substr(4);

  std::string html;
  html += "<button class=\"tab-btn folder-tab-btn\"";
  html += " data-folder-id=\"" + std::to_string(m.id) + "\"";
  html += " data-tab-id=\"" + tid + "\"";
  html += " data-folder-parent=\"" + std::to_string(m.parent_id) + "\"";
  html += " data-folder-name=\"" + htmlEscape(label) + "\"";
  html += " data-folder-icon=\"" + htmlEscape(icon) + "\"";
  html += " onclick=\"switchTab('tab-tiles-" + tid + "')\">";
  if (!icon.empty()) {
    html += "<i class=\"mdi mdi-" + htmlEscape(icon) + "\" style=\"font-size:24px;\"></i>";
  }
  html += "<span style=\"font-size:14px;font-weight:600;\">" + htmlEscape(label) + "</span>";
  html += "</button>";
  return html;
}

// Helper: add one type-specific field group div.
static void addTypeFields(std::string& h, const std::string& tid,
                          const std::string& group, const std::string& inner) {
  h += "<div id=\"" + tid + "_" + group + "_fields\" class=\"type-fields\">" + inner + "</div>";
}

// Helper: entity select (starts empty; admin.js populates it via /admin/entity_options).
static std::string entitySelect(const std::string& id) {
  return "<select id=\"" + id + "\"><option value=\"\">-- Select --</option></select>";
}

// Forward declaration (definition follows later, after GridTileData/readTileGrid).
static std::string buildTileGridHtml(const std::string& tid, int folder_id);

// Build the complete tab content HTML for one folder, including the tile-editor
// wrapper and the full tile-settings panel with all type-specific form fields.
// This is the equivalent of the HomeTiles appendTileTabHTML function.
static std::string buildFolderTabHtml(const FolderMeta& m) {
  const std::string tid   = "folder" + std::to_string(m.id);
  const std::string label = m.name.empty()
    ? (m.id == 0 ? "Home" : "Folder " + std::to_string(m.id + 1))
    : m.name;
  std::string icon = m.icon;
  if (icon.rfind("mdi:", 0) == 0) icon = icon.substr(4);
  else if (icon.rfind("mdi-", 0) == 0) icon = icon.substr(4);

  std::string h;
  // ── outer tab div ────────────────────────────────────────────────────────
  h += "<div id=\"tab-tiles-" + tid + "\" class=\"tab-content tile-tab\"";
  h += " data-tab-id=\"" + tid + "\"";
  h += " data-folder-id=\"" + std::to_string(m.id) + "\"";
  h += " data-folder-parent=\"" + std::to_string(m.parent_id) + "\"";
  h += " data-folder-name=\"" + htmlEscape(label) + "\"";
  h += " data-folder-icon=\"" + htmlEscape(icon) + "\">";

  // ── tile editor shell ────────────────────────────────────────────────────
  h += "<div class=\"tile-editor\">";
  h +=   "<div class=\"tile-editor-main\">";
  h +=     "<div class=\"tile-grid-scroll\">";
  h +=       buildTileGridHtml(tid, m.id);
  h +=     "</div>";
  h +=   "</div>";

  // ── settings panel ───────────────────────────────────────────────────────
  h += "<div class=\"tile-settings\" id=\"" + tid + "Settings\">";
  h +=   "<div class=\"tile-specific-settings hidden\">";

  // Head: type selector
  h +=     "<div class=\"tile-settings-head\">";
  h +=       "<h3 style=\"margin-top:0;\">Tile Settings</h3>";
  h +=       "<label>Type</label>";
  h +=       "<select id=\"" + tid + "_tile_type\"";
  h +=             " onchange=\"updateTileType('" + tid + "')\">";
  h +=         "<option value=\"0\">Empty</option>";
  h +=         "<option value=\"1\">Sensor</option>";
  h +=         "<option value=\"2\">Scene / Action</option>";
  h +=         "<option value=\"4\">Navigate</option>";
  h +=         "<option value=\"5\">Switch / Light</option>";
  h +=         "<option value=\"9\">Clock</option>";
  h +=         "<option value=\"10\">Text</option>";
  h +=         "<option value=\"12\">Weather</option>";
  h +=         "<option value=\"14\">Energy</option>";
  h +=         "<option value=\"15\">Media Player</option>";
  h +=         "<option value=\"16\">Animation</option>";
  h +=         "<option value=\"17\">Climate (AC)</option>";
  h +=         "<option value=\"18\">Camera</option>";
  h +=         "<option value=\"19\">Cover / Blind</option>";
  h +=       "</select>";
  h +=       "<p class=\"hint hidden\" id=\"" + tid + "_tile_type_hint\">Type locked</p>";
  h +=     "</div>";  // tile-settings-head

  // Body: common + type-specific fields
  h +=     "<div class=\"tile-settings-body\">";

  // Title
  h +=       "<label>Title</label>";
  h +=       "<input type=\"text\" id=\"" + tid + "_tile_title\" placeholder=\"Auto\">";

  // Icon
  h +=       "<label>Icon <small style=\"color:#8a8a8a;\">(Material Design)</small></label>";
  h +=       "<input type=\"text\" id=\"" + tid + "_tile_icon\" placeholder=\"e.g. thermometer\">";

  // Background colour
  h +=       "<div class=\"tile-color-label-row\"><span>Color</span></div>";
  h +=       "<div class=\"tile-color-row\">";
  h +=         "<input type=\"color\" id=\"" + tid + "_tile_color\" value=\"#2A2A2A\">";
  h +=         "<button type=\"button\" class=\"tile-color-reset-btn\"";
  h +=                " title=\"Reset\" onclick=\"resetTileColor('" + tid + "')\">";
  h +=           "<i class=\"mdi mdi-restore\"></i>";
  h +=         "</button>";
  h +=       "</div>";

  // Layout
  h +=       "<div class=\"tile-layout\">";
  auto layoutField = [&](const char* lbl, const char* sfx, int lo, int hi) {
    h += "<div class=\"layout-field\"><label>" + std::string(lbl) + "</label>";
    h += "<input type=\"number\" id=\"" + tid + "_tile_" + sfx + "\"";
    h += " min=\"" + std::to_string(lo) + "\" max=\"" + std::to_string(hi) + "\"";
    h += " step=\"1\" value=\"1\"></div>";
  };
  layoutField("Col",    "col",    1, 7);
  layoutField("Row",    "row",    1, 5);
  layoutField("Width",  "span_w", 1, 7);
  layoutField("Height", "span_h", 1, 5);
  h +=       "</div>";  // tile-layout

  // ── Sensor fields (type 1 = Sensor, type 9 = Gauge shares them) ──────────
  {
    std::string inner;
    inner += "<label>Sensor Entity</label>" + entitySelect(tid + "_sensor_entity");
    inner += "<label>Unit Override</label><input type=\"text\" id=\"" + tid + "_sensor_unit\" placeholder=\"Auto\">";
    inner += "<label>Decimals</label>";
    inner += "<select id=\"" + tid + "_sensor_decimals\">"
             "<option value=\"\">Auto</option>"
             "<option value=\"0\">0</option><option value=\"1\">1</option>"
             "<option value=\"2\">2</option><option value=\"3\">3</option>"
             "<option value=\"4\">4</option></select>";
    inner += "<label>Display Mode</label>";
    inner += "<select id=\"" + tid + "_sensor_display_mode\">"
             "<option value=\"0\">Value</option>"
             "<option value=\"1\">Gauge</option>"
             "<option value=\"2\">Bar</option>"
             "<option value=\"3\">Graph</option></select>";
    inner += "<div id=\"" + tid + "_sensor_gauge_fields\">";
    inner += "<label>Gauge Min</label><input type=\"number\" id=\"" + tid + "_sensor_gauge_min\" placeholder=\"0\">";
    inner += "<label>Gauge Max</label><input type=\"number\" id=\"" + tid + "_sensor_gauge_max\" placeholder=\"100\">";
    inner += "</div>";
    // Hidden inputs for less-used gauge fields (still needed by save/reset)
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_gauge_arc\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_gauge_size\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_gauge_y_offset\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_value_y_offset\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_graph_height\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_value_font\" value=\"0\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_sensor_popup_open_mode\" value=\"1\">";
    addTypeFields(h, tid, "sensor", inner);
  }

  // ── Energy fields (type 14) ───────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Energy Entity</label>" + entitySelect(tid + "_energy_entity");
    inner += "<input type=\"hidden\" id=\"" + tid + "_energy_popup_open_mode\" value=\"1\">";
    addTypeFields(h, tid, "energy", inner);
  }

  // ── Weather fields (type 12) ──────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Weather Entity</label>" + entitySelect(tid + "_weather_entity");
    addTypeFields(h, tid, "weather", inner);
  }

  // ── Scene fields (type 2) ─────────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Scene / Script</label>" + entitySelect(tid + "_scene_alias");
    addTypeFields(h, tid, "scene", inner);
  }

  // ── Navigate fields (type 4) ──────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Target Folder</label>";
    inner += "<select id=\"" + tid + "_navigate_target\">"
             "<option value=\"0\">-- Select --</option></select>";
    // Folder PIN (shown only when a real folder is selected by JS)
    inner += "<label class=\"inline-checkbox folder-pin-label\">"
             "<input type=\"checkbox\" id=\"" + tid + "_folder_pin_enabled\"> Require PIN</label>";
    inner += "<div class=\"folder-pin-control\">";
    inner += "<div class=\"password-field\">";
    inner += "<input type=\"password\" id=\"" + tid + "_folder_pin\" placeholder=\"PIN\">";
    inner += "<button type=\"button\" class=\"password-toggle\""
             " data-label-show=\"Show\" data-label-hide=\"Hide\">Show</button>";
    inner += "</div>";
    inner += "<button type=\"button\" id=\"" + tid + "_folder_pin_apply\""
             " onclick=\"applyFolderPin('" + tid + "')\">Save PIN</button>";
    inner += "</div>";
    inner += "<div id=\"" + tid + "_folder_pin_status\"></div>";
    addTypeFields(h, tid, "navigate", inner);
  }

  // ── Switch / Light fields (type 5) ────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Switch / Light Entity</label>" + entitySelect(tid + "_switch_entity");
    inner += "<label>Style</label>";
    inner += "<select id=\"" + tid + "_switch_style\">"
             "<option value=\"0\">Toggle</option>"
             "<option value=\"1\">Brightness Slider</option></select>";
    inner += "<input type=\"hidden\" id=\"" + tid + "_switch_popup_open_mode\" value=\"1\">";
    addTypeFields(h, tid, "switch", inner);
  }

  // ── Clock fields (type 9) ─────────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label class=\"inline-checkbox\">"
             "<input type=\"checkbox\" id=\"" + tid + "_clock_show_time\" checked> Show Time</label>";
    inner += "<label class=\"inline-checkbox\">"
             "<input type=\"checkbox\" id=\"" + tid + "_clock_show_date\"> Show Date</label>";
    inner += "<label class=\"inline-checkbox\">"
             "<input type=\"checkbox\" id=\"" + tid + "_clock_show_weekday\"> Show Weekday</label>";
    inner += "<label>Time Font Size</label>";
    inner += "<select id=\"" + tid + "_clock_time_font\">";
    for (int s : {20,24,28,32,40,48,56,64,72,80,96})
      inner += "<option value=\"" + std::to_string(s) + "\""
               + (s==40?" selected":"") + ">" + std::to_string(s) + "</option>";
    inner += "</select>";
    inner += "<label>Date Font Size</label>";
    inner += "<select id=\"" + tid + "_clock_date_font\">";
    for (int s : {12,14,16,18,20,24,28,32})
      inner += "<option value=\"" + std::to_string(s) + "\""
               + (s==20?" selected":"") + ">" + std::to_string(s) + "</option>";
    inner += "</select>";
    inner += "<label>Time Format</label>";
    inner += "<select id=\"" + tid + "_clock_time_format\">"
             "<option value=\"0\">Auto</option>"
             "<option value=\"1\">24 h</option>"
             "<option value=\"2\">12 h</option></select>";
    inner += "<label>Date Format</label>";
    inner += "<select id=\"" + tid + "_clock_date_format\">"
             "<option value=\"0\">Auto</option>"
             "<option value=\"1\">DD.MM.YYYY</option>"
             "<option value=\"2\">MM/DD/YYYY</option>"
             "<option value=\"3\">YYYY/MM/DD</option></select>";
    addTypeFields(h, tid, "clock", inner);
  }

  // ── Text fields (type 10) ─────────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Text</label>"
             "<input type=\"text\" id=\"" + tid + "_text_value\" placeholder=\"Enter text\">";
    inner += "<input type=\"hidden\" id=\"" + tid + "_text_value_font\" value=\"0\">";
    addTypeFields(h, tid, "text", inner);
  }

  // ── Media Player fields (type 15 = MEDIA_TILE_TYPE) ──────────────────────
  {
    std::string inner;
    inner += "<label>Media Player Entity</label>" + entitySelect(tid + "_media_entity");
    addTypeFields(h, tid, "media", inner);
  }

  // ── Animation fields (type 16) ────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Animation File</label>" + entitySelect(tid + "_animation_file");
    inner += "<label>Speed (FPS)</label>"
             "<input type=\"number\" id=\"" + tid + "_animation_fps\" min=\"1\" max=\"30\" value=\"10\">"
             "<span id=\"" + tid + "_animation_fps_val\">10</span>";
    inner += "<label>Zoom %</label>"
             "<input type=\"number\" id=\"" + tid + "_animation_zoom\" min=\"25\" max=\"300\" value=\"100\">"
             "<span id=\"" + tid + "_animation_zoom_val\">100</span>";
    inner += "<input type=\"hidden\" id=\"" + tid + "_animation_fit\" value=\"0\">";
    addTypeFields(h, tid, "animation", inner);
  }

  // ── Climate / AC fields (type 17) ─────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Climate Entity</label>" + entitySelect(tid + "_climate_entity");
    addTypeFields(h, tid, "climate", inner);
  }

  // ── Camera fields (type 18) ───────────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Camera Entity</label>" + entitySelect(tid + "_camera_entity");
    addTypeFields(h, tid, "camera", inner);
  }

  // ── Cover / Blind fields (type 19) ────────────────────────────────────────
  {
    std::string inner;
    inner += "<label>Cover Entity</label>" + entitySelect(tid + "_cover_entity");
    addTypeFields(h, tid, "cover", inner);
  }

  h +=     "</div>";  // tile-settings-body

  // Action buttons
  h +=     "<div class=\"tile-actions\">";
  h +=       "<button type=\"button\" class=\"btn\" onclick=\"copyTile('" + tid + "')\">Copy</button>";
  h +=       "<button type=\"button\" class=\"btn\" onclick=\"pasteTile('" + tid + "')\">Paste</button>";
  h +=       "<button type=\"button\" class=\"btn btn-danger\" onclick=\"resetTile('" + tid + "')\">Delete</button>";
  h +=     "</div>";

  h +=   "</div>";  // tile-specific-settings
  h += "</div>";    // tile-settings

  h += "</div>";  // tile-editor
  h += "</div>";  // outer tab div

  return h;
}

// Read tile array from /sdcard/_tile_grids/f{id}.json.
// Returns a vector of 35 raw JSON objects (or empty objects for missing tiles).
struct GridTileData {
  int   type    = 0;
  int   col     = 0, row = 0, span_w = 1, span_h = 1;
  unsigned int bg = 0;  // 0 = no custom colour
  std::string icon, title;
  int navigate_target = 0;
};

static std::vector<GridTileData> readTileGrid(int folder_id) {
  constexpr int COLS = 7, ROWS = 5, TOTAL = 35;
  std::vector<GridTileData> out(TOTAL);
  // Assign default grid positions
  for (int i = 0; i < TOTAL; i++) {
    out[i].col = i % COLS;
    out[i].row = i / COLS;
  }

  char path[80];
  snprintf(path, sizeof(path), "/sdcard/_tile_grids/f%d.json", folder_id);
  FILE *f = fopen(path, "rb");
  if (!f) return out;

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz <= 0 || sz > 65536) { fclose(f); return out; }

  std::string raw(static_cast<size_t>(sz), '\0');
  fread(&raw[0], 1, static_cast<size_t>(sz), f);
  fclose(f);

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return out;

  // Accept bare array [] or legacy {"tiles":[...]}
  JsonArray arr;
  if (doc.is<JsonArray>())           arr = doc.as<JsonArray>();
  else if (doc["tiles"].is<JsonArray>()) arr = doc["tiles"].as<JsonArray>();
  else return out;

  int idx = 0;
  for (JsonObject t : arr) {
    if (idx >= TOTAL) break;
    GridTileData& d = out[idx];
    d.type         = t["type"]          | 0;
    d.col          = t["col"]           | (idx % COLS);
    d.row          = t["row"]           | (idx / COLS);
    d.span_w       = t["span_w"]        | 1;
    d.span_h       = t["span_h"]        | 1;
    d.bg           = t["bg_color"]      | 0u;
    d.icon         = t["icon_name"]     | "";
    d.title        = t["title"]         | "";
    d.navigate_target = t["navigate_target"] | 0;
    // Clamp span
    if (d.span_w < 1) d.span_w = 1;
    if (d.span_h < 1) d.span_h = 1;
    if (d.col + d.span_w > COLS) d.span_w = COLS - d.col;
    if (d.row + d.span_h > ROWS) d.span_h = ROWS - d.row;
    idx++;
  }
  return out;
}

// Build the 35 tile placeholder divs for the tile-grid.
// Each tile gets an onclick that triggers selectTile() in admin.js.
static std::string buildTileGridHtml(const std::string& tid, int folder_id) {
  const auto tiles = readTileGrid(folder_id);
  std::string h;
  h += "<div class=\"tile-grid\">";

  for (int i = 0; i < (int)tiles.size(); i++) {
    const GridTileData& t = tiles[i];

    std::string cls = "tile";
    if (t.type == 0) cls += " empty";

    // Background colour
    std::string style;
    style += "grid-column:" + std::to_string(t.col + 1) + "/span " + std::to_string(t.span_w) + ";";
    style += "grid-row:"    + std::to_string(t.row + 1) + "/span " + std::to_string(t.span_h) + ";";
    if (t.type != 0) {
      char hex[16];
      unsigned int rgb = t.bg & 0xFFFFFF;
      if (rgb == 0) rgb = 0x353535;  // default background
      snprintf(hex, sizeof(hex), "#%06X", rgb);
      style += "background:";
      style += hex;
      style += ";";
    } else {
      style += "background:transparent;";
    }

    h += "<div class=\"" + cls + "\"";
    h += " id=\"" + tid + "-tile-" + std::to_string(i) + "\"";
    h += " data-index=\"" + std::to_string(i) + "\"";
    h += " data-type=\""  + std::to_string(t.type) + "\"";
    h += " data-col=\""   + std::to_string(t.col) + "\"";
    h += " data-row=\""   + std::to_string(t.row) + "\"";
    h += " data-span-w=\"" + std::to_string(t.span_w) + "\"";
    h += " data-span-h=\"" + std::to_string(t.span_h) + "\"";
    if (t.type == 4)  // Navigate tile
      h += " data-navigate-target=\"" + std::to_string(t.navigate_target) + "\"";
    h += " draggable=\"true\"";
    h += " style=\"" + style + "\"";
    h += " onclick=\"selectTile(parseInt(this.dataset.index),'" + tid + "')\"";
    h += " ondblclick=\"openPreviewNavigation(this,'" + tid + "')\">";

    if (t.type != 0) {
      // Icon
      std::string icon = t.icon;
      for (auto& c : icon) c = std::tolower((unsigned char)c);
      if (icon.rfind("mdi:", 0) == 0) icon = icon.substr(4);
      else if (icon.rfind("mdi-", 0) == 0) icon = icon.substr(4);
      if (!icon.empty())
        h += "<i class=\"mdi mdi-" + icon + " tile-icon\"></i>";

      // Title
      if (!t.title.empty())
        h += "<div class=\"tile-title\" id=\"" + tid + "-tile-" + std::to_string(i) + "-title\">"
             + htmlEscape(t.title) + "</div>";

      // Resize handle (needed for drag-resize)
      h += "<span class=\"tile-resize-handle\"></span>";
    }

    h += "</div>";
  }

  h += "</div>";  // tile-grid
  return h;
}

// ── TilesHandler ─────────────────────────────────────────────────────────────

TilesHandler::TilesHandler(const std::string &base) : base_(base) {}

bool TilesHandler::canHandle(AsyncWebServerRequest *request) const {
  const auto url = request_url(request);
  ESP_LOGD("web_admin_local.tiles", "canHandle TilesHandler: url=%s method=%d base=%s", url.c_str(), request->method(), base_.c_str());
  const std::string prefix = base_ + "/tiles";
  // Only serve the HTML page for bare GET requests without a 'folder' query parameter.
  // Requests with ?folder=N are handled by ApiTilesHandler as JSON API calls.
  return request->method() == HTTP_GET
      && (url == prefix || url == prefix + "/")
      && !request->hasParam("folder");
}

void TilesHandler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI("web_admin_local.tiles", "handleRequest TilesHandler: %s", request_url(request).c_str());
  std::string adminJs = std::string(adminJsAssetPath());
  std::string adminCss = std::string(adminCssAssetPath());

  // Read folder metadata (name, icon) from SD card; fall back to defaults.
  auto folders = readFolderMetaList();

  // Generate tab nav buttons and tab content using the full HTML builder.
  std::string nav_buttons;
  std::string tab_divs;
  for (const auto& m : folders) {
    nav_buttons += buildFolderButtonHtml(m);
    tab_divs    += buildFolderTabHtml(m);
  }

  // First folder tab id for auto-switch
  std::string first_tab = "folder" + std::to_string(folders[0].id);

  // ── Required global constants for admin.js ────────────────────────────────
  // TILE_TYPE_REGISTRY maps each tile type number to its label, the CSS field-
  // group name shown in updateTileType(), and the global handler function names
  // that callTypeHandler() looks up on window.  All handler functions are
  // already defined in admin.js – we only need to wire them up here.
  static const char kGlobals[] =
    "<script>"
    "var APP_I18N={};"
    "var APP_LOCALE='en';"
    "var GRID_COLS=7;"
    "var GRID_ROWS=5;"
    "var TILES_PER_GRID=35;"
    "var SCREENSAVER_FOLDER_ID=100;"
    "var ADMIN_WEB_SESSION_TOKEN='local';"
    "var MEDIA_TILE_TYPE=15;"
    "var MEDIA_TILE_MIN_SPAN=2;"
    "var MEDIA_TILE_MAX_SPAN=4;"
    "var SCREENSAVER_TILE_DEFAULT_OPACITY=0;"
    "var NAVIGATE_I18N={};"
    "var TILE_TYPE_REGISTRY={"
      // 0 = empty
      "'0':{label:'Empty'},"
      // 1 = sensor / value display
      "'1':{label:'Sensor',fields:'sensor',"
            "load:'loadSensorFields',save:'saveSensorFields',reset:'resetSensorFields'},"
      // 2 = scene / script
      "'2':{label:'Scene',fields:'scene',"
            "load:'loadSceneFields',save:'saveSceneFields',reset:'resetSceneFields'},"
      // 4 = navigate to folder
      "'4':{label:'Navigate',fields:'navigate',"
            "load:'loadNavigateFields',save:'saveNavigateFields',reset:'resetNavigateFields'},"
      // 5 = switch / light toggle
      "'5':{label:'Switch',fields:'switch',"
            "load:'loadSwitchFields',save:'saveSwitchFields',reset:'resetSwitchFields'},"
      // 7 = network-settings tile (locked, no user config)
      "'7':{label:'Network',locked:true},"
      // 8 = back-navigation tile (locked)
      "'8':{label:'Back',locked:true},"
      // 9 = clock
      "'9':{label:'Clock',fields:'clock',"
            "load:'loadClockFields',save:'saveClockFields',reset:'resetClockFields'},"
      // 10 = static text
      "'10':{label:'Text',fields:'text',"
             "load:'loadTextFields',save:'saveTextFields',reset:'resetTextFields'},"
      // 12 = weather
      "'12':{label:'Weather',fields:'weather',"
             "load:'loadWeatherFields',save:'saveWeatherFields',reset:'resetWeatherFields'},"
      // 14 = energy / consumption
      "'14':{label:'Energy',fields:'energy',"
             "load:'loadEnergyFields',save:'saveEnergyFields',reset:'resetEnergyFields'},"
      // 15 = media player (= MEDIA_TILE_TYPE)
      "'15':{label:'Media',fields:'media',"
             "load:'loadMediaFields',save:'saveMediaFields',reset:'resetMediaFields'},"
      // 16 = lottie / gif animation
      "'16':{label:'Animation',fields:'animation',"
             "load:'loadAnimationFields',save:'saveAnimationFields',reset:'resetAnimationFields'},"
      // 17 = climate / AC thermostat
      "'17':{label:'Climate',fields:'climate',"
             "load:'loadClimateFields',save:'saveClimateFields',reset:'resetClimateFields'},"
      // 18 = IP camera snapshot
      "'18':{label:'Camera',fields:'camera',"
             "load:'loadCameraFields',save:'saveCameraFields',reset:'resetCameraFields'},"
      // 19 = cover / blind / garage door
      "'19':{label:'Cover',fields:'cover',"
             "load:'loadCoverFields',save:'saveCoverFields',reset:'resetCoverFields'}"
    "};"
    "</script>";

  std::string html =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    // ── CSS custom properties needed by admin.css ─────────────────────────
    // Grid preview dimensions (7×5 layout for the standard Waveshare P4 board).
    // Cell height: (430 - 2*12 - 10*(5-1)) / 5 = 73px; width scaled proportionally.
    "<style>:root{"
      "--grid-cols:7;--grid-rows:5;"
      "--preview-cell-w:80px;--preview-cell-h:73px;"
      "--preview-gap:10px;--preview-pad:12px;"
      "--settings-panel-width:390px;"
      "--admin-wrapper-width:1200px;"
      // Tile font sizes (scaled ~75% of LVGL px for web preview)
      "--fs16:12px;--fs20:15px;--fs24:18px;--fs28:21px;"
      "--fs32:24px;--fs40:30px;--fs48:36px;--fs56:42px;--fs64:48px;--fs72:54px;"
      // Tile layout
      "--tile-title-font:11px;--tile-value-font:18px;"
      // Climate
      "--climate-grid-gap:5px;"
    "}</style>"
    // MDI icon font (icons on tiles, buttons, resize handles)
    "<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/@mdi/font@7.4.47/css/materialdesignicons.min.css\">"
    "<link rel=\"stylesheet\" href=\"" + adminCss + "\">"
    "</head><body>"
    "<div class=\"wrapper\"><div class=\"card\">"
    "<nav class=\"tab-nav\">" + nav_buttons + "</nav>"
    + tab_divs
    // Required anchor: insertBefore(tabEl, networkTab) in installFolderTabFragment
    + "<div id=\"tab-network\" class=\"tab-content\"></div>"
    // Notification banner: showNotification() writes here
    + "<div id=\"notification\" class=\"notification\"></div>"
    // Hidden settings slot used for the special network-settings tile (type 7)
    + "<div id=\"settingsHiddenSlot\" class=\"settings-hidden-slot\" draggable=\"false\">"
      "<div id=\"settingsHiddenTile\" class=\"tile hidden-settings-tile\" draggable=\"true\">"
        "<span id=\"settingsHiddenHint\"></span>"
      "</div>"
    "</div>"
    + "</div></div>"
    + kGlobals
    + "<script src=\"" + adminJs + "\"></script>"
    "<script>"
    // Detect admin.js scope availability before doing anything
    "console.log('[admin-local] typeof selectTile=',typeof selectTile,"
      "'typeof switchTab=',typeof switchTab,"
      "'typeof applyTileDataToEditor=',typeof applyTileDataToEditor);"
    // Explicit window exports — only if admin.js made them accessible
    "window.switchTab=typeof switchTab==='function'?switchTab:window.switchTab;"
    "window.selectTile=typeof selectTile==='function'?selectTile:window.selectTile;"
    "window.openPreviewNavigation=typeof openPreviewNavigation==='function'?openPreviewNavigation:window.openPreviewNavigation;"
    "window.updateTileType=typeof updateTileType==='function'?updateTileType:window.updateTileType;"
    "window.resetTileColor=typeof resetTileColor==='function'?resetTileColor:window.resetTileColor;"
    "window.copyTile=typeof copyTile==='function'?copyTile:window.copyTile;"
    "window.pasteTile=typeof pasteTile==='function'?pasteTile:window.pasteTile;"
    "window.resetTile=typeof resetTile==='function'?resetTile:window.resetTile;"
    "window.applyFolderPin=typeof applyFolderPin==='function'?applyFolderPin:window.applyFolderPin;"
    // Standalone selectTile — works even when admin.js functions are not global.
    // If admin.js's selectTile WAS exported above, this is NOT executed (guard below).
    "if(typeof window.selectTile!=='function'){"
      "window.selectTile=function(index,tab){"
        // Visual: mark tile, deselect others
        "document.querySelectorAll('.tile-grid>.tile').forEach(function(t){"
          "t.classList.remove('active');delete t.dataset.selected;});"
        "var tileEl=document.getElementById(tab+'-tile-'+index);"
        "if(tileEl){tileEl.classList.add('active');tileEl.dataset.selected='1';}"
        // Show settings panel
        "var sp=document.getElementById(tab+'Settings');"
        "if(sp){var ts=sp.querySelector('.tile-specific-settings');if(ts)ts.classList.remove('hidden');}"
        // Fetch tile data and populate form
        "var fid=parseInt((tab.match(/\\d+$/)||['0'])[0],10);"
        "fetch('/admin/tiles?folder='+fid+'&index='+index)"
          ".then(function(r){return r.json();})"
          ".then(function(data){window._fillTileSettings(tab,data);})"
          ".catch(function(e){console.warn('tile data:',e);});"
        // Also load entity options
        "fetch('/admin/entity_options')"
          ".then(function(r){return r.json();})"
          ".then(function(d){window._entityOptions=d;})"
          ".catch(function(){});"
      "};"
    "}"
    // Standalone form population — populates settings panel from raw tile data object.
    "window._fillTileSettings=function(tab,data){"
      "if(!data||typeof data!=='object'||Array.isArray(data))return;"
      "var g=function(id){return document.getElementById(tab+'_'+id);};"
      // Type selector
      "var tsel=g('tile_type');"
      "if(tsel){"
        "var tv=String(data.type||0);"
        "if(!Array.from(tsel.options).some(function(o){return o.value===tv;})){"
          "var op=document.createElement('option');op.value=tv;op.textContent='Type '+tv;tsel.appendChild(op);}"
        "tsel.value=tv;"
      "}"
      // Title, icon
      "var ti=g('tile_title');if(ti)ti.value=data.title||'';"
      "var ic=g('tile_icon');if(ic)ic.value=data.icon_name||'';"
      // Color
      "var co=g('tile_color');"
      "if(co&&data.bg_color&&data.bg_color!==0){"
        "co.value='#'+('000000'+((data.bg_color||0)&0xFFFFFF).toString(16)).slice(-6);}"
      // Layout
      "var cl=g('tile_col');if(cl)cl.value=(Number(data.col)||0)+1;"
      "var rw=g('tile_row');if(rw)rw.value=(Number(data.row)||0)+1;"
      "var sw=g('tile_span_w');if(sw)sw.value=data.span_w||1;"
      "var sh=g('tile_span_h');if(sh)sh.value=data.span_h||1;"
      // Update type-specific field visibility
      "if(typeof updateTileType==='function')updateTileType(tab);"
      // Entity selects: add current value as option, then select it
      "var emap=["
        "['sensor_entity',data.sensor_entity],"
        "['switch_entity',data.switch_entity||data.sensor_entity],"
        "['weather_entity',data.weather_entity||data.sensor_entity],"
        "['energy_entity',data.energy_entity||data.sensor_entity],"
        "['media_entity',data.media_entity||data.sensor_entity],"
        "['climate_entity',data.climate_entity||data.sensor_entity],"
        "['cover_entity',data.cover_entity||data.sensor_entity],"
        "['camera_entity',data.camera_entity||data.sensor_entity],"
        "['scene_alias',data.scene_alias]"
      "];"
      "emap.forEach(function(p){"
        "if(!p[1])return;"
        "var sel=g(p[0]);if(!sel)return;"
        // Populate from entity options cache if available
        "var opts=(window._entityOptions&&window._entityOptions[p[0].replace('_entity','')+'s'])||[];"
        "opts.forEach(function(opt){"
          "if(!Array.from(sel.options).some(function(o){return o.value===opt.v;})){"
            "var o=document.createElement('option');o.value=opt.v;o.textContent=opt.t||opt.v;"
            "sel.appendChild(o);}});"
        // Add current value if missing
        "if(!Array.from(sel.options).some(function(o){return o.value===p[1];})){"
          "var o=document.createElement('option');o.value=p[1];o.textContent=p[1];sel.appendChild(o);}"
        "sel.value=p[1];"
      "});"
    "};"
    // Patch applyTileDataToEditor for configuredValue fix (if admin.js IS accessible)
    "if(typeof applyTileDataToEditor==='function'){"
      "var _adt=applyTileDataToEditor;"
      "window.applyTileDataToEditor=applyTileDataToEditor=function(idx,tab,data){"
        "_adt(idx,tab,data);"
        "if(!data||typeof data!=='object'||Array.isArray(data))return;"
        "["
          "[tab+'_sensor_entity',data.sensor_entity],"
          "[tab+'_switch_entity',data.switch_entity||data.sensor_entity],"
          "[tab+'_weather_entity',data.weather_entity||data.sensor_entity],"
          "[tab+'_energy_entity',data.energy_entity||data.sensor_entity],"
          "[tab+'_media_entity',data.media_entity||data.sensor_entity],"
          "[tab+'_climate_entity',data.climate_entity||data.sensor_entity],"
          "[tab+'_cover_entity',data.cover_entity||data.sensor_entity],"
          "[tab+'_camera_entity',data.camera_entity||data.sensor_entity],"
          "[tab+'_scene_alias',data.scene_alias]"
        "].forEach(function(pair){"
          "if(!pair[1])return;"
          "var el=document.getElementById(pair[0]);"
          "if(el)el.dataset.configuredValue=pair[1];"
        "});"
      "};"
    "}"
    // Standalone updateTileType — shows/hides type-specific fields.
    // Uses TILE_TYPE_REGISTRY which is always defined in our kGlobals script.
    "if(typeof window.updateTileType!=='function'){"
      "window.updateTileType=function(tab){"
        "document.querySelectorAll('#'+tab+'Settings .type-fields')"
          ".forEach(function(f){f.classList.remove('show');});"
        "var sel=document.getElementById(tab+'_tile_type');"
        "var tv=sel?sel.value:'0';"
        "var meta=window.TILE_TYPE_REGISTRY&&window.TILE_TYPE_REGISTRY[tv];"
        "if(meta&&meta.fields){"
          "var fe=document.getElementById(tab+'_'+meta.fields+'_fields');"
          "if(fe)fe.classList.add('show');"
        "}"
      "};"
    "}"
    // Standalone resetTileColor
    "if(typeof window.resetTileColor!=='function'){"
      "window.resetTileColor=function(tab){"
        "var el=document.getElementById(tab+'_tile_color');if(el)el.value='#2A2A2A';"
      "};"
    "}"
    // Standalone copyTile, pasteTile, resetTile (clipboard in localStorage)
    "if(typeof window.copyTile!=='function'){"
      "window.copyTile=function(tab){window._tileClipboard=window._collectTileForm(tab);console.log('copied');};"
    "}"
    "if(typeof window.pasteTile!=='function'){"
      "window.pasteTile=function(tab){if(window._tileClipboard)window._fillTileSettings(tab,window._tileClipboard);};"
    "}"
    "if(typeof window.resetTile!=='function'){"
      "window.resetTile=function(tab){"
        // Find currently selected tile index
        "var tileEl=document.querySelector('#tab-tiles-'+tab+' .tile-grid>.tile.active');"
        "if(!tileEl){console.warn('resetTile: no tile selected');return;}"
        "var idx=parseInt(tileEl.dataset.index,10);"
        "if(isNaN(idx))return;"
        // Clear form fields
        "window._fillTileSettings(tab,{type:0,title:'',icon_name:'',bg_color:0});"
        // Calculate folder ID from tab name
        "var fid=parseInt((tab.match(/\\d+$/)||['0'])[0],10);"
        // POST empty tile to server
        "var fd=new FormData();"
        "fd.append('folder',fid);"
        "fd.append('index',idx);"
        "fd.append('type','0');"
        "fd.append('title','');"
        "fd.append('icon_name','');"
        "fd.append('bg_color_default','1');"
        // Use tile's actual layout position
        "fd.append('col',tileEl.dataset.col||String(idx%7));"
        "fd.append('row',tileEl.dataset.row||String(Math.floor(idx/7)));"
        "fd.append('span_w','1');"
        "fd.append('span_h','1');"
        "fetch('/admin/tiles',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:new URLSearchParams(fd)})"
          ".then(function(r){return r.json();})"
          ".then(function(d){"
            "if(d.success){"
              // Update tile visual: make it empty
              "tileEl.style.background='transparent';"
              "tileEl.dataset.type='0';"
              "tileEl.classList.add('empty');"
              "tileEl.classList.remove('active');"
              "delete tileEl.dataset.selected;"
              "tileEl.innerHTML='';"
              // Hide settings panel
              "var sp=document.getElementById(tab+'Settings');"
              "if(sp){var ts=sp.querySelector('.tile-specific-settings');if(ts)ts.classList.add('hidden');}"
              "console.log('Tile',idx,'deleted in',tab);"
            "}"
          "})"
          ".catch(function(e){console.warn('delete failed',e);});"
      "};"
    "}"
    "if(typeof window.applyFolderPin!=='function'){"
      "window.applyFolderPin=function(tab){console.warn('applyFolderPin not implemented');};"
    "}"
    "if(typeof window.openPreviewNavigation!=='function'){"
      "window.openPreviewNavigation=function(el,tab){};"
    "}"
    // Collect current form values for copyTile
    "window._collectTileForm=function(tab){"
      "var g=function(id){return document.getElementById(tab+'_'+id);};"
      "var d={};"
      "var ts=g('tile_type');if(ts)d.type=Number(ts.value)||0;"
      "var ti=g('tile_title');if(ti)d.title=ti.value;"
      "var ic=g('tile_icon');if(ic)d.icon_name=ic.value;"
      "return d;"
    "};"
    // Auto-save: when any settings form field changes, POST the updated tile.
    // Runs after DOMContentLoaded so all settings panels exist.
    "document.addEventListener('DOMContentLoaded',function(){"
      "function _saveTileFromForm(tab,idx){"
        "var g=function(id){return document.getElementById(tab+'_'+id);};"
        "var fid=parseInt((tab.match(/\\d+$/)||['0'])[0],10);"
        "var fd=new FormData();"
        "fd.append('folder',fid);fd.append('index',idx);"
        "var ts=g('tile_type');fd.append('type',ts?ts.value:'0');"
        "var ti=g('tile_title');fd.append('title',ti?ti.value:'');"
        "var ic=g('tile_icon');fd.append('icon_name',ic?ic.value:'');"
        "var co=g('tile_color');"
        "if(co&&co.value&&co.value!=='#2a2a2a'&&co.value!=='#2A2A2A'){"
          "var rgb=parseInt(co.value.replace('#',''),16);"
          "fd.append('bg_color',rgb);"
        "}else{fd.append('bg_color_default','1');}"
        "var cl=g('tile_col');fd.append('col',cl?(Number(cl.value)||1)-1:0);"
        "var rw=g('tile_row');fd.append('row',rw?(Number(rw.value)||1)-1:0);"
        "var sw=g('tile_span_w');fd.append('span_w',sw?sw.value:'1');"
        "var sh=g('tile_span_h');fd.append('span_h',sh?sh.value:'1');"
        "['sensor_entity','sensor_unit','sensor_decimals','sensor_display_mode',"
        "'switch_entity','switch_style','navigate_target','scene_alias',"
        "'weather_entity','energy_entity','media_entity','climate_entity',"
        "'cover_entity','camera_entity','animation_file','animation_fps',"
        "'clock_show_time','clock_show_date','clock_time_format','clock_date_format',"
        "'text_value'].forEach(function(f){"
          "var el=document.getElementById(tab+'_'+f);"
          "if(el&&el.value!==undefined)fd.append(f,el.value);"
        "});"
        "fetch('/admin/tiles',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:new URLSearchParams(fd)})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.success)console.log('saved tile',idx,'in',tab);})"
          ".catch(function(e){console.warn('save failed',e);});"
      "}"
      // Wire change handlers to all settings panels
      "document.querySelectorAll('.tile-settings').forEach(function(sp){"
        "var tabMatch=sp.id.replace('Settings','');"
        "if(!tabMatch)return;"
        "var saveTimer=null;"
        "sp.addEventListener('change',function(e){"
          "var sel=document.getElementById(tabMatch+'_tile_type');"
          "if(e.target===sel&&typeof window.updateTileType==='function'){"
            "window.updateTileType(tabMatch);"
          "}"
          "var tileEl=document.querySelector('#tab-tiles-'+tabMatch+' .tile-grid>.tile.active');"
          "var idx=tileEl?parseInt(tileEl.dataset.index,10):-1;"
          "if(idx<0)return;"
          "clearTimeout(saveTimer);"
          "saveTimer=setTimeout(function(){_saveTileFromForm(tabMatch,idx);},400);"
        "});"
      "});"
    "});"
    // Pure-CSS fallback tab switcher - no admin.js dependency
    "window._swTab=function(name){"
      "document.querySelectorAll('.tab-content').forEach(function(e){e.classList.remove('active');});"
      "document.querySelectorAll('.tab-btn').forEach(function(e){e.classList.remove('active');});"
      "var t=document.getElementById(name);if(t)t.classList.add('active');"
      "var id=name.indexOf('tab-tiles-')===0?name.slice(10):name;"
      "var b=document.querySelector('.folder-tab-btn[data-tab-id=\"'+id+'\"]');"
      "if(b)b.classList.add('active');"
    "};"
    // Ensure window.switchTab is ALWAYS callable — use admin.js version if accessible,
    // otherwise fall back to _swTab so onclick='switchTab(...)' never throws.
    "window.switchTab=typeof switchTab==='function'?switchTab:window._swTab;"
    // Activate first tab if admin.js did not do it
    "document.addEventListener('DOMContentLoaded',function(){"
      "if(document.querySelector('.tab-content.active'))return;"
      "if(typeof initTileTabs==='function')initTileTabs();"
      "var tid=(typeof tileTabs!=='undefined'&&tileTabs[0])||'folder0';"
      "(window.switchTab||window._swTab)('tab-tiles-'+tid);"
    "});"
    // Wire every folder button with a direct click listener as fallback
    "document.addEventListener('DOMContentLoaded',function(){"
      "document.querySelectorAll('.folder-tab-btn').forEach(function(btn){"
        "btn.addEventListener('click',function(){"
          "var name='tab-tiles-'+(btn.dataset.tabId||'folder0');"
          "(window.switchTab||window._swTab)(name);"
        "});"
      "});"
    "});"
    // Enable drag+resize on all tile tabs; safe to call again if admin.js already did it
    // (enableTileResize guards against double-binding; enableTileDrag just adds extra listeners)
    "document.addEventListener('DOMContentLoaded',function(){"
      "setTimeout(function(){"
        "if(typeof enableTileDrag==='function'&&typeof tileTabs!=='undefined'){"
          "tileTabs.forEach(function(t){enableTileDrag(t);enableTileResize(t);});"
        "}"
        "if(typeof updateTileSettingsMaxHeight==='function')updateTileSettingsMaxHeight();"
        // Tile-grid click delegation: catches tile clicks in all grids.
        // window.selectTile is our standalone implementation above.
        "document.querySelectorAll('.tile-grid').forEach(function(grid){"
          "if(grid.dataset.tileClickBound==='1')return;"
          "grid.dataset.tileClickBound='1';"
          "grid.addEventListener('click',function(e){"
            "var tile=e.target.closest('.tile');"
            "if(!tile)return;"
            "var tab=tile.id.replace(/-tile-\\d+$/,'');"
            "var idx=parseInt(tile.dataset.index,10);"
            "if(tab&&!isNaN(idx)&&typeof window.selectTile==='function')"
              "window.selectTile(idx,tab);"
          "});"
        "});"
      "},150);"
    "});"
    "</script>"
    "</body></html>";

  // Always serve fresh HTML so cached pages don't show stale function references.
  auto *resp = request->beginResponse(200, "text/html; charset=utf-8", html.c_str());
  resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(resp);
}

// ApiTilesHandler implementation with JSON-backed storage
ApiTilesHandler::ApiTilesHandler(const std::string &base) : base_(base) {}

bool ApiTilesHandler::canHandle(AsyncWebServerRequest *request) const {
  const auto url = request_url(request);
  ESP_LOGD("web_admin_local.api.tiles", "canHandle ApiTilesHandler: url=%s method=%d base=%s", url.c_str(), request->method(), base_.c_str());
  const std::string apiPrefix = std::string("/api") + "/tiles";
  const std::string adminPrefix = base_ + "/tiles";
  bool match = (url == apiPrefix || url == apiPrefix + "/" || url == adminPrefix || url == adminPrefix + "/");
  if (!match) return false;
  // POST always goes to the API handler (saving tile config).
  if (request->method() == HTTP_POST) return true;
  // GET only goes to the API handler when a 'folder' param is present.
  // Bare GET requests (browser page loads) are served as HTML by TilesHandler.
  if (request->method() == HTTP_GET) return request->hasParam("folder");
  return false;
}

// Helper to get folder id from query string
static uint16_t get_folder_id(AsyncWebServerRequest *request) {
  if (request->hasArg("folder")) {
    const std::string s = request->arg("folder");
    char *endptr = nullptr;
    long v = strtol(s.c_str(), &endptr, 10);
    if (endptr == s.c_str()) return 0; // no conversion
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    return static_cast<uint16_t>(v);
  }
  return 0;
}

void ApiTilesHandler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI("web_admin_local.api.tiles", "handleRequest ApiTilesHandler: method=%d url=%s",
           request->method(), request_url(request).c_str());
  uint16_t folder = get_folder_id(request);
  // Files are stored on the SD card under /sdcard/_tile_grids/
  char pathBuf[80];
  snprintf(pathBuf, sizeof(pathBuf), "/sdcard/_tile_grids/f%u.json", static_cast<unsigned>(folder));
  const char* filePath = pathBuf;

  if (request->method() == HTTP_GET) {
    ESP_LOGD("web_admin_local.api.tiles", "GET -> %s", filePath);
    FILE *rf = fopen(filePath, "rb");
    if (rf) {
      if (fseek(rf, 0, SEEK_END) != 0) {
        fclose(rf);
        request->send(500, "text/plain", "Failed to read tiles file");
        return;
      }
      long sz = ftell(rf);
      rewind(rf);
      if (sz < 0) sz = 0;
      std::string raw;
      raw.resize(static_cast<size_t>(sz));
      if (sz > 0) fread(&raw[0], 1, static_cast<size_t>(sz), rf);
      fclose(rf);

      // The SPA expects a bare JSON array [].
      // Old files may be stored as {"tiles":[...]}; unwrap transparently.
      JsonDocument doc;
      JsonArray arr;
      if (!deserializeJson(doc, raw)) {
        if (doc.is<JsonArray>()) {
          arr = doc.as<JsonArray>();
        } else if (doc["tiles"].is<JsonArray>()) {
          arr = doc["tiles"].as<JsonArray>();
        }
      }

      // If ?index=N is requested, return a single tile object for selectTile().
      // The SPA calls /admin/tiles?folder=N&index=I to get tile-specific data
      // when the full grid hasn't been loaded yet.
      if (request->hasArg("index")) {
        const std::string idxStr = request->arg("index");
        char *ep = nullptr;
        long idx = strtol(idxStr.c_str(), &ep, 10);
        if (ep != idxStr.c_str() && idx >= 0) {
          std::string tileJson = "{}";
          if (arr && static_cast<size_t>(idx) < arr.size()) {
            serializeJson(arr[static_cast<size_t>(idx)], tileJson);
          }
          request->send(200, "application/json; charset=utf-8", tileJson.c_str());
          return;
        }
      }

      // No index: return the full array.
      if (arr) {
        std::string arrStr;
        serializeJson(arr, arrStr);
        request->send(200, "application/json; charset=utf-8", arrStr.c_str());
      } else {
        request->send(200, "application/json; charset=utf-8", raw.c_str());
      }
      return;
    }
    ESP_LOGD("web_admin_local.api.tiles", "File does not exist: %s", filePath);
    // If index was requested but file doesn't exist, return empty tile object
    if (request->hasArg("index")) {
      request->send(200, "application/json; charset=utf-8", "{}");
    } else {
      request->send(200, "application/json; charset=utf-8", "[]");
    }
    return;
  }

  if (request->method() == HTTP_POST) {
    ESP_LOGD("web_admin_local.api.tiles", "POST to %s", filePath);

    // ── Form-data tile save (admin.js saveTile) ───────────────────────────────
    // admin.js sends multipart/form-data with fields: folder, index, col, row,
    // span_w, span_h, type, title, icon_name, bg_color / bg_color_default, and
    // any type-specific entity / config fields.
    if (request->hasArg("index")) {
      ESP_LOGD("web_admin_local.api.tiles", "FormData tile save, folder=%u", (unsigned)folder);

      const std::string idxStr = request->arg("index");
      char *idxEp = nullptr;
      long tile_idx = strtol(idxStr.c_str(), &idxEp, 10);
      if (idxEp == idxStr.c_str() || tile_idx < 0 || tile_idx >= 35) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"bad index\"}");
        return;
      }

      // Load (or create) the current grid file.
      // Use a flat approach: load into array, expand to 35 elements, update.
      std::vector<std::string> tileStrings(35, "{}");
      {
        FILE *rf = fopen(filePath, "rb");
        if (rf) {
          fseek(rf, 0, SEEK_END);
          long sz = ftell(rf); rewind(rf);
          if (sz > 0 && sz <= 65536) {
            std::string raw(static_cast<size_t>(sz), '\0');
            fread(&raw[0], 1, static_cast<size_t>(sz), rf);
            JsonDocument rdoc;
            if (!deserializeJson(rdoc, raw)) {
              JsonArray rarr;
              if (rdoc.is<JsonArray>()) rarr = rdoc.as<JsonArray>();
              else if (rdoc["tiles"].is<JsonArray>()) rarr = rdoc["tiles"].as<JsonArray>();
              if (rarr) {
                int ri = 0;
                for (JsonObject t : rarr) {
                  if (ri >= 35) break;
                  serializeJson(t, tileStrings[ri]);
                  ri++;
                }
              }
            }
          }
          fclose(rf);
        }
      }

      // Parse and update the specific tile.
      JsonDocument tdoc;
      if (deserializeJson(tdoc, tileStrings[tile_idx])) tdoc.clear();
      JsonObject tile = tdoc.is<JsonObject>() ? tdoc.as<JsonObject>() : tdoc.to<JsonObject>();

      // Helpers: write a numeric or string field from a request arg.
      auto setIntField = [&](const char *argName, const char *key) {
        if (!request->hasArg(argName)) return;
        const std::string v = request->arg(argName);
        if (v.empty()) return;
        char *ep = nullptr;
        long n = strtol(v.c_str(), &ep, 10);
        if (ep != v.c_str()) tile[key] = (int)n;
      };
      auto setStrField = [&](const char *argName, const char *key) {
        if (!request->hasArg(argName)) return;
        tile[key] = request->arg(argName);
      };
      auto setFloatField = [&](const char *argName, const char *key) {
        if (!request->hasArg(argName)) return;
        const std::string v = request->arg(argName);
        if (v.empty()) return;
        char *ep = nullptr;
        double d = strtod(v.c_str(), &ep);
        if (ep != v.c_str()) tile[key] = d;
      };

      // Standard tile fields
      setIntField("type",    "type");
      setStrField("title",   "title");
      setStrField("icon_name", "icon_name");
      setIntField("col",     "col");
      setIntField("row",     "row");
      setIntField("span_w",  "span_w");
      setIntField("span_h",  "span_h");
      if (request->hasArg("bg_color_default")) {
        tile["bg_color"] = 0;
      } else {
        setIntField("bg_color", "bg_color");
      }
      setIntField("background_opacity", "background_opacity");

      // Type-specific string fields
      static const char *strFields[] = {
        "sensor_entity","sensor_unit","switch_entity","navigate_target",
        "scene_alias","weather_entity","energy_entity","media_entity",
        "climate_entity","cover_entity","camera_entity","animation_file",
        "climate_geometry","text_value","key_macro","clock_show_time","clock_show_date",
        nullptr
      };
      for (int i = 0; strFields[i]; i++) setStrField(strFields[i], strFields[i]);

      // Type-specific numeric fields
      static const char *numFields[] = {
        "sensor_decimals","sensor_value_font","sensor_display_mode",
        "sensor_gauge_min","sensor_gauge_max","sensor_gauge_arc",
        "sensor_gauge_size","sensor_gauge_y_offset","sensor_value_y_offset",
        "sensor_graph_height","popup_open_mode",
        "switch_style","animation_fps","animation_fit","animation_zoom",
        "clock_time_format","clock_date_format","key_code","key_modifier",
        "text_value_font","climate_slots_packed","climate_layouts_packed",
        nullptr
      };
      for (int i = 0; numFields[i]; i++) setFloatField(numFields[i], numFields[i]);

      // Serialize updated tile back into the strings array.
      tileStrings[tile_idx].clear();
      serializeJson(tile, tileStrings[tile_idx]);

      // Rebuild the full JSON array from all tile strings.
      mkdir("/sdcard/_tile_grids", 0755);
      std::string out = "[";
      for (int i = 0; i < 35; i++) {
        if (i) out += ",";
        out += tileStrings[i];
      }
      out += "]";
      std::string tmp = std::string(filePath) + ".tmp";
      FILE *wf = fopen(tmp.c_str(), "wb");
      if (!wf) {
        request->send(500, "application/json", "{\"success\":false,\"error\":\"write failed\"}");
        return;
      }
      fwrite(out.data(), 1, out.size(), wf);
      fclose(wf);
      remove(filePath);
      rename(tmp.c_str(), filePath);

      // Defer the rebuild to WebAdminLocal::loop(); this handler runs on the
      // HTTP task and LVGL must only be mutated from the ESPHome loop task.
      if (g_tiles_renderer) {
        g_tiles_renderer->request_refresh_folder(static_cast<int>(folder));
      }

      // Build response — include navigate_target so the SPA can update folder links.
      std::string resp = "{\"success\":true";
      if (request->hasArg("navigate_target")) {
        resp += ",\"navigate_target\":";
        resp += request->arg("navigate_target");
      }
      resp += "}";
      ESP_LOGI("web_admin_local.api.tiles", "Saved tile %ld in folder %u", tile_idx, (unsigned)folder);
      request->send(200, "application/json; charset=utf-8", resp.c_str());
      return;
    }

    // ── JSON body import / test-tool POST ─────────────────────────────────────
    // body_buf_ is populated by handleBody() before handleRequest().
    std::string body = std::move(body_buf_);
    body_buf_.clear();

    if (body_too_large_) {
      body_too_large_ = false;
      ESP_LOGW("web_admin_local.api.tiles", "Request body exceeds maximum size");
      request->send(413, "text/plain", "Request body too large");
      return;
    }
    if (body.empty()) {
      ESP_LOGW("web_admin_local.api.tiles", "No body found in request");
      request->send(400, "text/plain", "Missing body");
      return;
    }
    // Basic JSON validation
    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err) {
      ESP_LOGW("web_admin_local.api.tiles", "JSON parse error: %s", err.c_str());
      request->send(400, "text/plain", "JSON parse error");
      return;
    }

    // Support two payload formats and always store a bare JSON array [].
    // 1) { "tiles": [ ... ] }  <-- test-tool / existing format
    // 2) Waveshare export: { "grids": { "0": [...], "1": [...] } }
    std::string body_to_save;
    if (doc["tiles"].is<JsonArray>()) {
      serializeJson(doc["tiles"], body_to_save);
    } else if (doc["grids"].is<JsonObject>()) {
      // When the export includes folder metadata, persist it so names/icons
      // are available to TilesHandler and FoldersApiHandler.
      if (doc["folders"].is<JsonArray>()) {
        std::string folderJson = "{\"folders\":";
        serializeJson(doc["folders"], folderJson);
        folderJson += "}";
        mkdir("/sdcard/_tile_grids", 0755);
        FILE *ff = fopen("/sdcard/_tile_grids/folders.json", "wb");
        if (ff) {
          fwrite(folderJson.data(), 1, folderJson.size(), ff);
          fclose(ff);
        }
      }
      // Extract the requested folder grid
      char folder_key[16];
      snprintf(folder_key, sizeof(folder_key), "%u", static_cast<unsigned>(folder));
      if (doc["grids"][folder_key].is<JsonArray>()) {
        serializeJson(doc["grids"][folder_key], body_to_save);
      } else {
        if (doc["grids"]["0"].is<JsonArray>()) {
          serializeJson(doc["grids"]["0"], body_to_save);
        } else {
          ESP_LOGW("web_admin_local.api.tiles", "No grid found for folder %u", static_cast<unsigned>(folder));
          request->send(400, "text/plain", "No grid for folder in JSON");
          return;
        }
      }
    } else {
      ESP_LOGW("web_admin_local.api.tiles", "Missing tiles array in JSON");
      request->send(400, "text/plain", "Missing tiles array or grids export");
      return;
    }

    // Ensure the directory exists (create if needed)
    mkdir("/sdcard/_tile_grids", 0755);

    // Write to temporary file and atomically rename using stdio
    std::string tmpPath = std::string(filePath) + ".tmp";
    FILE *wf = fopen(tmpPath.c_str(), "wb");
    if (!wf) {
      ESP_LOGW("web_admin_local.api.tiles", "Failed to open tmp file for write: %s", tmpPath.c_str());
      request->send(500, "text/plain", "Failed to open temp file");
      return;
    }
    size_t written = fwrite(body_to_save.data(), 1, body_to_save.size(), wf);
    fclose(wf);
    if (written != body_to_save.size()) {
      ESP_LOGW("web_admin_local.api.tiles", "Incomplete write to tmp file: wrote %u expected %u", (unsigned)written, (unsigned)body_to_save.size());
      remove(tmpPath.c_str());
      request->send(500, "text/plain", "Failed to write file");
      return;
    }
    // Try atomic rename
    remove(filePath);
    if (rename(tmpPath.c_str(), filePath) != 0) {
      ESP_LOGW("web_admin_local.api.tiles", "Rename failed, trying copy fallback");
      // Copy fallback
      FILE *src = fopen(tmpPath.c_str(), "rb");
      FILE *dst = fopen(filePath, "wb");
      if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        remove(tmpPath.c_str());
        request->send(500, "text/plain", "Failed to finalize save");
        return;
      }
      char buf[256];
      size_t r;
      while ((r = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, r, dst);
      }
      fclose(src);
      fclose(dst);
      remove(tmpPath.c_str());
    }

    ESP_LOGI("web_admin_local.api.tiles", "Saved tiles to %s", filePath);
    // Rebuild LVGL page for this folder
    if (g_tiles_renderer) {
      g_tiles_renderer->request_refresh_folder(static_cast<int>(folder));
    }
    request->send(200, "application/json; charset=utf-8", "{\"status\":\"ok\"}");
    return;
  }

  request->send(405, "text/plain", "Method Not Allowed");
}

void ApiTilesHandler::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  static constexpr size_t kMaxTileRequestBody = 128 * 1024;
  if (index == 0) {
    body_buf_.clear();
    body_too_large_ = total > kMaxTileRequestBody;
    if (!body_too_large_) body_buf_.reserve(total);
  }
  if (!body_too_large_) {
    if (body_buf_.size() > kMaxTileRequestBody - std::min(len, kMaxTileRequestBody))
      body_too_large_ = true;
    else
      body_buf_.append(reinterpret_cast<const char *>(data), len);
  }
  ESP_LOGD("web_admin_local.api.tiles", "handleBody: index=%u len=%u total=%u accumulated=%u",
           (unsigned)index, (unsigned)len, (unsigned)total, (unsigned)body_buf_.size());
}

// ── FoldersApiHandler ────────────────────────────────────────────────────────

FoldersApiHandler::FoldersApiHandler(const std::string &base) : base_(base) {}

bool FoldersApiHandler::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET) return false;
  const auto url = request_url(request);
  // Serve under the admin prefix (e.g. /admin/folders) AND the SPA's
  // hard-coded /api/folders paths so the built-in export/import still works.
  const std::string adminBase = base_ + "/folders";
  const std::string apiBase   = std::string("/api/folders");
  return url == adminBase       || url == adminBase + "/"
      || url == adminBase + "/tab" || url == adminBase + "/tab/"
      || url == apiBase         || url == apiBase + "/"
      || url == apiBase + "/tab"   || url == apiBase + "/tab/";
}

// Build the JSON for /api/folders/tab?folder_id=N.
// Returns {"success":true,"tab_id":"folderN","button_html":"...","tab_html":"..."}.
std::string FoldersApiHandler::buildFolderTabJson(int folder_id, const std::string& /*unused_name*/) {
  const FolderMeta m = getFolderMeta(folder_id);
  const std::string tab_id = "folder" + std::to_string(folder_id);

  const std::string button_html = buildFolderButtonHtml(m);
  const std::string tab_html    = buildFolderTabHtml(m);

  // Escape double-quotes inside HTML strings for JSON embedding.
  auto jsonEsc = [](const std::string &s) {
    std::string out;
    out.reserve(s.size() + 32);
    for (char c : s) {
      if      (c == '"')  out += "\\\"";
      else if (c == '\\') out += "\\\\";
      else if (c == '\n') out += "\\n";
      else if (c == '\r') /* skip */;
      else                 out += c;
    }
    return out;
  };

  return std::string("{\"success\":true,\"tab_id\":\"") + tab_id
    + "\",\"button_html\":\"" + jsonEsc(button_html)
    + "\",\"tab_html\":\"" + jsonEsc(tab_html) + "\"}";
}

void FoldersApiHandler::handleRequest(AsyncWebServerRequest *request) {
  const auto url = request_url(request);
  const bool is_tab = url.find("/tab") != std::string::npos;

  // GET /admin/folders or /api/folders — folder list from files on disk.
  // Format: {"success":true,"folders":[{"id":0,"parent_id":0,"name":"Home","icon_name":""},...]}
  if (!is_tab) {
    auto metas = readFolderMetaList();
    std::string json = "{\"success\":true,\"folders\":[";
    bool first = true;
    for (const auto& m : metas) {
      if (!first) json += ",";
      first = false;
      json += "{\"id\":" + std::to_string(m.id);
      json += ",\"parent_id\":" + std::to_string(m.parent_id);
      json += ",\"name\":\"";
      // JSON-escape the name
      for (char c : m.name) {
        if      (c == '"')  json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else                json += c;
      }
      json += "\",\"icon_name\":\"";
      for (char c : m.icon) {
        if      (c == '"')  json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else                json += c;
      }
      json += "\",\"pin_enabled\":false}";
    }
    json += "]}";
    request->send(200, "application/json; charset=utf-8", json.c_str());
    return;
  }

  // GET /admin/folders/tab or /api/folders/tab?folder_id=N — tab+button HTML for dynamic loading.
  if (is_tab) {
    int folder_id = 0;
    if (request->hasArg("folder_id")) {
      const std::string s = request->arg("folder_id");
      char *ep = nullptr;
      long v = strtol(s.c_str(), &ep, 10);
      if (ep != s.c_str() && v >= 0 && v <= 9) folder_id = static_cast<int>(v);
    }
    const std::string name = (folder_id == 0) ? "Home" : ("Folder " + std::to_string(folder_id + 1));
    const std::string json = buildFolderTabJson(folder_id, name);
    request->send(200, "application/json; charset=utf-8", json.c_str());
    return;
  }

  request->send(404, "text/plain", "Not Found");
}

// ── EntityOptionsHandler ─────────────────────────────────────────────────────
// Serves /admin/entity_options — scans all stored tile grids and returns
// the entity IDs already in use, grouped by tile type.  This allows the
// admin.js entity-select dropdowns to show the entities that are
// configured even without a live Home-Assistant bridge connection.

EntityOptionsHandler::EntityOptionsHandler(const std::string &base, const std::string &home_assistant_url,
                                           const std::string &home_assistant_token)
    : base_(base), home_assistant_url_(home_assistant_url), home_assistant_token_(home_assistant_token) {}

bool EntityOptionsHandler::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET) return false;
  const auto url = request_url(request);
  const std::string adminPath = base_ + "/entity_options";
  const std::string apiPath   = "/api/entity_options";
  return url == adminPath || url == adminPath + "/" ||
         url == apiPath   || url == apiPath + "/";
}

void EntityOptionsHandler::handleRequest(AsyncWebServerRequest *request) {
  if (home_assistant_url_.empty() || home_assistant_token_.empty()) {
    request->send(503, "application/json; charset=utf-8",
                  "{\"success\":false,\"error\":\"Home Assistant REST API is not configured\"}");
    return;
  }

  struct EntitySet {
    std::vector<std::string> sensors, switches, weathers, energy,
                              media, climates, covers, cameras, scenes;
    void add(std::vector<std::string> &v, const std::string &s) {
      if (s.empty()) return;
      for (auto &e : v) { if (e == s) return; }
      v.push_back(s);
    }
  } es;

  struct HttpResponse {
    std::string scan;
    std::vector<std::string> entity_ids;
    int status = 0;
  } response;
  static constexpr size_t kMaxEntityIds = 256;
  response.scan.reserve(1024);
  std::string url = home_assistant_url_;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/states";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 5000;
  config.user_data = &response;
  config.event_handler = [](esp_http_client_event_t *event) {
    auto *result = static_cast<HttpResponse *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
      result->scan.append(static_cast<const char *>(event->data),
                          static_cast<size_t>(event->data_len));
      size_t search = 0;
      while ((search = result->scan.find("\"entity_id\"", search)) != std::string::npos) {
        const size_t colon = result->scan.find(':', search + 11);
        if (colon == std::string::npos) break;
        const size_t first_quote = result->scan.find('"', colon + 1);
        if (first_quote == std::string::npos) break;
        const size_t last_quote = result->scan.find('"', first_quote + 1);
        if (last_quote == std::string::npos) break;
        const std::string id = result->scan.substr(first_quote + 1, last_quote - first_quote - 1);
        const size_t dot = id.find('.');
        if (dot != std::string::npos && dot > 0 && result->entity_ids.size() < kMaxEntityIds) {
          const std::string domain = id.substr(0, dot);
          if (domain == "sensor" || domain == "binary_sensor" || domain == "switch" ||
              domain == "light" || domain == "input_boolean" || domain == "weather" ||
              domain == "energy" || domain == "media_player" || domain == "climate" ||
              domain == "cover" || domain == "camera" || domain == "scene" || domain == "script") {
            bool duplicate = false;
            for (const auto &existing : result->entity_ids) {
              if (existing == id) {
                duplicate = true;
                break;
              }
            }
            if (!duplicate) result->entity_ids.push_back(id);
          }
        }
        result->scan.erase(0, last_quote + 1);
        search = 0;
      }
      if (result->scan.size() > 1024)
        result->scan.erase(0, result->scan.size() - 1024);
    }
    return ESP_OK;
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    request->send(502, "application/json; charset=utf-8",
                  "{\"success\":false,\"error\":\"Unable to initialize Home Assistant REST client\"}");
    return;
  }
  std::string auth = "Bearer " + home_assistant_token_;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Accept", "application/json");
  const esp_err_t result = esp_http_client_perform(client);
  response.status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || response.status != 200) {
    request->send(502, "application/json; charset=utf-8",
                  "{\"success\":false,\"error\":\"Home Assistant REST API request failed\"}");
    return;
  }
  for (const std::string &entity_id : response.entity_ids) {
    const size_t dot = entity_id.find('.');
    if (dot == std::string::npos || dot == 0) continue;
    const std::string domain = entity_id.substr(0, dot);
    if (domain == "sensor" || domain == "binary_sensor") es.add(es.sensors, entity_id);
    else if (domain == "switch" || domain == "light" || domain == "input_boolean") es.add(es.switches, entity_id);
    else if (domain == "weather") es.add(es.weathers, entity_id);
    else if (domain == "energy") es.add(es.energy, entity_id);
    else if (domain == "media_player") es.add(es.media, entity_id);
    else if (domain == "climate") es.add(es.climates, entity_id);
    else if (domain == "cover") es.add(es.covers, entity_id);
    else if (domain == "camera") es.add(es.cameras, entity_id);
    else if (domain == "scene" || domain == "script") es.add(es.scenes, entity_id);
  }

  // Build response in the format admin.js expects:
  // {"success":true,"sensors":[{"v":"id","t":"id"}],...}
  auto appendList = [](std::string &json, const char *key,
                       const std::vector<std::string> &list) {
    json += "\"";
    json += key;
    json += "\":[";
    bool first = true;
    for (const auto &e : list) {
      if (!first) json += ",";
      first = false;
      json += "{\"v\":\"";
      for (char c : e) {
        if (c == '"' || c == '\\') json += '\\';
        json += c;
      }
      json += "\",\"t\":\"";
      // Use entity ID as display text (no HA bridge available)
      for (char c : e) {
        if (c == '"' || c == '\\') json += '\\';
        json += c;
      }
      json += "\"}";
    }
    json += "]";
  };

  std::string json = "{\"success\":true,";
  appendList(json, "sensors",  es.sensors);  json += ",";
  appendList(json, "switches", es.switches); json += ",";
  appendList(json, "weathers", es.weathers); json += ",";
  appendList(json, "energy",   es.energy);   json += ",";
  appendList(json, "media",    es.media);    json += ",";
  appendList(json, "climates", es.climates); json += ",";
  appendList(json, "covers",   es.covers);   json += ",";
  appendList(json, "cameras",  es.cameras);  json += ",";
  appendList(json, "scenes",   es.scenes);
  json += "}";

  request->send(200, "application/json; charset=utf-8", json.c_str());
}

} // namespace web_admin_local
