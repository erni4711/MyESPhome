#include "config/components/uitiles/model.h"
#include <cstring>

namespace uitiles {

// JSON helpers
void tileToJsonObject(const Tile& tile, JsonObject& obj) {
  obj["type"] = static_cast<uint8_t>(tile.type);
  obj["title"] = tile.title.c_str();
  obj["icon_name"] = tile.icon_name.c_str();
  obj["bg_color"] = tile.bg_color;
  obj["background_opacity"] = tile.background_opacity;

  obj["col"] = tile.col;
  obj["row"] = tile.row;
  obj["span_w"] = tile.span_w;
  obj["span_h"] = tile.span_h;

  obj["sensor_entity"] = tile.sensor_entity.c_str();
  obj["sensor_unit"] = tile.sensor_unit.c_str();
  obj["sensor_decimals"] = tile.sensor_decimals;
  obj["sensor_value_font"] = tile.sensor_value_font;
  obj["sensor_display_mode"] = tile.sensor_display_mode;
  obj["sensor_gauge_min"] = tile.sensor_gauge_min;
  obj["sensor_gauge_max"] = tile.sensor_gauge_max;
  obj["sensor_gauge_arc"] = tile.sensor_gauge_arc;
  obj["sensor_gauge_size"] = tile.sensor_gauge_size;
  obj["sensor_gauge_y_offset"] = tile.sensor_gauge_y_offset;
  obj["sensor_value_y_offset"] = tile.sensor_value_y_offset;
  obj["sensor_graph_height"] = tile.sensor_graph_height;
  obj["popup_open_mode"] = tile.popup_open_mode;

  obj["scene_alias"] = tile.scene_alias.c_str();

  obj["key_macro"] = tile.key_macro.c_str();
  obj["key_code"] = tile.key_code;
  obj["key_modifier"] = tile.key_modifier;

  obj["image_path"] = tile.image_path.c_str();
  obj["image_slideshow_sec"] = tile.image_slideshow_sec;
}

bool tileFromJsonObject(const JsonObject& obj, Tile& tile) {
  if (!obj.containsKey("type")) return false;
  tile.type = static_cast<TileType>(obj["type"].as<uint8_t>());
  tile.title = obj["title"].as<const char*>() ? String(obj["title"].as<const char*>()) : String("");
  tile.icon_name = obj["icon_name"].as<const char*>() ? String(obj["icon_name"].as<const char*>()) : String("");
  tile.bg_color = obj["bg_color"].as<uint32_t>();
  tile.background_opacity = obj.containsKey("background_opacity") ? obj["background_opacity"].as<uint8_t>() : 255;

  tile.col = obj.containsKey("col") ? obj["col"].as<uint8_t>() : 0;
  tile.row = obj.containsKey("row") ? obj["row"].as<uint8_t>() : 0;
  tile.span_w = obj.containsKey("span_w") ? obj["span_w"].as<uint8_t>() : 1;
  tile.span_h = obj.containsKey("span_h") ? obj["span_h"].as<uint8_t>() : 1;

  tile.sensor_entity = obj.containsKey("sensor_entity") && obj["sensor_entity"].as<const char*>() ? String(obj["sensor_entity"].as<const char*>()) : String("");
  tile.sensor_unit = obj.containsKey("sensor_unit") && obj["sensor_unit"].as<const char*>() ? String(obj["sensor_unit"].as<const char*>()) : String("");
  tile.sensor_decimals = obj.containsKey("sensor_decimals") ? obj["sensor_decimals"].as<uint8_t>() : 0xFF;
  tile.sensor_value_font = obj.containsKey("sensor_value_font") ? obj["sensor_value_font"].as<uint8_t>() : 0;
  tile.sensor_display_mode = obj.containsKey("sensor_display_mode") ? obj["sensor_display_mode"].as<uint8_t>() : 0;
  tile.sensor_gauge_min = obj.containsKey("sensor_gauge_min") ? obj["sensor_gauge_min"].as<int32_t>() : 0;
  tile.sensor_gauge_max = obj.containsKey("sensor_gauge_max") ? obj["sensor_gauge_max"].as<int32_t>() : 100;
  tile.sensor_gauge_arc = obj.containsKey("sensor_gauge_arc") ? obj["sensor_gauge_arc"].as<uint16_t>() : 100;
  tile.sensor_gauge_size = obj.containsKey("sensor_gauge_size") ? obj["sensor_gauge_size"].as<uint16_t>() : 350;
  tile.sensor_gauge_y_offset = obj.containsKey("sensor_gauge_y_offset") ? obj["sensor_gauge_y_offset"].as<int16_t>() : 12;
  tile.sensor_value_y_offset = obj.containsKey("sensor_value_y_offset") ? obj["sensor_value_y_offset"].as<int16_t>() : 0;
  tile.sensor_graph_height = obj.containsKey("sensor_graph_height") ? obj["sensor_graph_height"].as<uint16_t>() : 60;
  tile.popup_open_mode = obj.containsKey("popup_open_mode") ? obj["popup_open_mode"].as<uint8_t>() : static_cast<uint8_t>(TILE_POPUP_OPEN_LONG_PRESS);

  tile.scene_alias = obj.containsKey("scene_alias") && obj["scene_alias"].as<const char*>() ? String(obj["scene_alias"].as<const char*>()) : String("");

  tile.key_macro = obj.containsKey("key_macro") && obj["key_macro"].as<const char*>() ? String(obj["key_macro"].as<const char*>()) : String("");
  tile.key_code = obj.containsKey("key_code") ? obj["key_code"].as<uint8_t>() : 0;
  tile.key_modifier = obj.containsKey("key_modifier") ? obj["key_modifier"].as<uint8_t>() : 0;

  tile.image_path = obj.containsKey("image_path") && obj["image_path"].as<const char*>() ? String(obj["image_path"].as<const char*>()) : String("");
  tile.image_slideshow_sec = obj.containsKey("image_slideshow_sec") ? obj["image_slideshow_sec"].as<uint16_t>() : 10;

  return true;
}

// UiTilesModel
UiTilesModel::UiTilesModel(fs::FS& storage_fs) : fs_(storage_fs) {}

// JSON IO
bool UiTilesModel::saveGridToJsonFile(const String& path, const std::vector<Tile>& grid) {
  DynamicJsonDocument doc(32768);
  JsonObject root = doc.to<JsonObject>();
  root["version"] = 7;
  JsonArray tiles = root.createNestedArray("tiles");
  for (const auto& t : grid) {
    JsonObject obj = tiles.createNestedObject();
    tileToJsonObject(t, obj);
  }
  File f = fs_.open(path.c_str(), FILE_WRITE);
  if (!f) return false;
  if (serializeJsonPretty(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

bool UiTilesModel::loadGridFromJsonFile(const String& path, std::vector<Tile>& out_grid) {
  if (!fs_.exists(path.c_str())) return false;
  File f = fs_.open(path.c_str(), FILE_READ);
  if (!f) return false;
  size_t size = f.size();
  if (size == 0) { f.close(); return false; }
  size_t alloc = size + 1024;
  if (alloc > 65536) alloc = 65536;
  DynamicJsonDocument doc(alloc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonObject root = doc.as<JsonObject>();
  JsonArray tiles = root["tiles"].as<JsonArray>();
  if (!tiles) return false;
  out_grid.clear();
  out_grid.reserve(tiles.size());
  for (JsonVariant v : tiles) {
    JsonObject obj = v.as<JsonObject>();
    Tile t;
    if (!tileFromJsonObject(obj, t)) return false;
    out_grid.push_back(t);
  }
  return true;
}

// Helpers
void UiTilesModel::copy_cstr_from_String(const String& src, char* dst, size_t max_len) {
  if (!dst || max_len == 0) return;
  memset(dst, 0, max_len);
  if (src.length() == 0) return;
  size_t n = src.length();
  if (n >= max_len) n = max_len - 1;
  memcpy(dst, src.c_str(), n);
}

String UiTilesModel::cstr_to_String(const char* src) {
  if (!src) return String("");
  return String(src);
}

// Path helpers (static)
String UiTilesModel::tileGridFile(uint16_t folder_id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/f%05u_v7.bin", "_tile_grids", static_cast<unsigned>(folder_id));
  return String(buf);
}

String UiTilesModel::imagePathFile(uint16_t folder_id, size_t index) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/f%u_%02u.url", "_tile_links", static_cast<unsigned>(folder_id), static_cast<unsigned>(index));
  return String(buf);
}

String UiTilesModel::entityPathFile(uint16_t folder_id, size_t index) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/f%u_%02u.ent", "_tile_entities", static_cast<unsigned>(folder_id), static_cast<unsigned>(index));
  return String(buf);
}

bool UiTilesModel::replaceFileWithPreparedTmp(fs::FS& fs, const String& tmpPath, const String& filePath) {
  // This private helper was moved into class methods; not used externally here.
  if (!fs.exists(tmpPath.c_str())) return false;
  const String backupPath = filePath + ".bak";
  if (fs.exists(backupPath.c_str())) fs.remove(backupPath.c_str());
  if (fs.rename(tmpPath.c_str(), filePath.c_str())) {
    return true;
  }
  if (fs.exists(filePath.c_str())) {
    if (!fs.rename(filePath.c_str(), backupPath.c_str())) {
      return false;
    }
  }
  if (fs.rename(tmpPath.c_str(), filePath.c_str())) {
    if (fs.exists(backupPath.c_str())) fs.remove(backupPath.c_str());
    return true;
  }
  if (fs.exists(backupPath.c_str()) && !fs.exists(filePath.c_str())) {
    fs.rename(backupPath.c_str(), filePath.c_str());
  }
  return false;
}

void UiTilesModel::packTileToV7(const Tile& src, PackedTileV7& dst, uint16_t folder_id, size_t index) {
  memset(&dst, 0, sizeof(dst));
  dst.type = static_cast<uint8_t>(src.type);
  dst.sensor_decimals = src.sensor_decimals;
  dst.key_code = src.key_code;
  dst.key_modifier = src.key_modifier;
  dst.bg_color = src.bg_color;
  dst.col = src.col;
  dst.row = src.row;
  dst.span_w = src.span_w;
  dst.span_h = src.span_h;
  copy_cstr_from_String(src.title, dst.title, TITLE_MAX);
  copy_cstr_from_String(src.icon_name, dst.icon_name, ICON_MAX);

  if (src.sensor_entity.length() >= ENTITY_MAX) {
    String p = entityPathFile(folder_id, index);
    File f = fs_.open(p.c_str(), FILE_WRITE);
    if (f) {
      f.print(src.sensor_entity.c_str());
      f.close();
    }
    dst.sensor_entity[0] = '\0';
  } else {
    copy_cstr_from_String(src.sensor_entity, dst.sensor_entity, ENTITY_MAX);
    String p = entityPathFile(folder_id, index);
    if (fs_.exists(p.c_str())) fs_.remove(p.c_str());
  }

  copy_cstr_from_String(src.sensor_unit, dst.sensor_unit, UNIT_MAX);
  copy_cstr_from_String(src.scene_alias, dst.scene_alias, SCENE_MAX);
  copy_cstr_from_String(src.key_macro, dst.key_macro, MACRO_MAX);
  dst.sensor_value_font = src.sensor_value_font;
  dst.image_slideshow_sec = src.image_slideshow_sec;
  dst.sensor_gauge_enabled = (src.sensor_gauge_max != src.sensor_gauge_min) ? 1 : 0;
  dst.sensor_gauge_min = src.sensor_gauge_min;
  dst.sensor_gauge_max = src.sensor_gauge_max;
  dst.popup_open_mode = src.popup_open_mode;

  if (src.image_path.length() > 0 && src.image_path.length() >= 128) {
    String p = imagePathFile(folder_id, index);
    File f = fs_.open(p.c_str(), FILE_WRITE);
    if (f) {
      f.print(src.image_path.c_str());
      f.close();
    }
    dst.image_slideshow_sec = src.image_slideshow_sec;
  } else {
    dst.image_slideshow_sec = src.image_slideshow_sec;
    String p = imagePathFile(folder_id, index);
    if (fs_.exists(p.c_str())) fs_.remove(p.c_str());
  }
}

void UiTilesModel::unpackV7ToTile(const PackedTileV7& src, Tile& dst, uint16_t folder_id, size_t index) {
  dst.type = static_cast<TileType>(src.type);
  dst.sensor_decimals = src.sensor_decimals;
  dst.key_code = src.key_code;
  dst.key_modifier = src.key_modifier;
  dst.bg_color = src.bg_color;
  dst.col = src.col;
  dst.row = src.row;
  dst.span_w = src.span_w;
  dst.span_h = src.span_h;
  dst.title = cstr_to_String(src.title);
  dst.icon_name = cstr_to_String(src.icon_name);

  String entPath = entityPathFile(folder_id, index);
  if (fs_.exists(entPath.c_str())) {
    File f = fs_.open(entPath.c_str(), FILE_READ);
    if (f) {
      String content;
      while (f.available()) content += (char)f.read();
      f.close();
      dst.sensor_entity = content;
    }
  } else {
    dst.sensor_entity = cstr_to_String(src.sensor_entity);
  }

  dst.sensor_unit = cstr_to_String(src.sensor_unit);
  dst.scene_alias = cstr_to_String(src.scene_alias);
  dst.key_macro = cstr_to_String(src.key_macro);
  dst.sensor_value_font = src.sensor_value_font;
  dst.image_slideshow_sec = src.image_slideshow_sec;
  dst.sensor_gauge_max = src.sensor_gauge_max;
  dst.sensor_gauge_min = src.sensor_gauge_min;
  dst.popup_open_mode = src.popup_open_mode;
}

void UiTilesModel::packGridToV7(const std::vector<Tile>& grid, PackedQuarterGridV7* out_packed, uint16_t folder_id) {
  for (size_t q = 0; q < QUARTERS_PER_GRID; ++q) {
    PackedQuarterGridV7& pq = out_packed[q];
    memset(&pq, 0, sizeof(pq));
    pq.version = PACKED_GRID_VERSION;
    pq.quarter_index = static_cast<uint8_t>(q);
    for (size_t ti = 0; ti < TILES_PER_QUARTER; ++ti) {
      size_t global_index = q * TILES_PER_QUARTER + ti;
      if (global_index < grid.size()) {
        packTileToV7(grid[global_index], pq.tiles[ti], folder_id, global_index);
      } else {
        memset(&pq.tiles[ti], 0, sizeof(PackedTileV7));
      }
    }
  }
}

bool UiTilesModel::writePackedGridFile(uint16_t folder_id, const PackedQuarterGridV7* packed, size_t count) {
  if (!fs_.exists("/_tile_grids")) {
    fs_.mkdir("/_tile_grids");
  }
  String filePath = tileGridFile(folder_id);
  String tmpPath = filePath + ".tmp";
  if (fs_.exists(tmpPath.c_str())) fs_.remove(tmpPath.c_str());

  File f = fs_.open(tmpPath.c_str(), FILE_WRITE);
  if (!f) return false;
  const size_t expected = count * sizeof(PackedQuarterGridV7);
  size_t written = f.write(reinterpret_cast<const uint8_t*>(packed), expected);
  f.flush();
  f.close();
  if (written != expected) {
    fs_.remove(tmpPath.c_str());
    return false;
  }
  // atomic replace
  if (!replaceFileWithPreparedTmp(fs_, tmpPath, filePath)) {
    fs_.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool UiTilesModel::readPackedGridFile(const String& filePath, PackedQuarterGridV7* packed, size_t count) {
  if (!fs_.exists(filePath.c_str())) return false;
  File f = fs_.open(filePath.c_str(), FILE_READ);
  if (!f) return false;
  const size_t expected = count * sizeof(PackedQuarterGridV7);
  size_t sz = f.size();
  if (sz < expected) { f.close(); return false; }
  size_t read = f.read(reinterpret_cast<uint8_t*>(packed), expected);
  f.close();
  return read == expected;
}

bool UiTilesModel::saveGridToBinaryFile(uint16_t folder_id, const std::vector<Tile>& grid) {
  std::vector<PackedQuarterGridV7> packed(QUARTERS_PER_GRID);
  packGridToV7(grid, packed.data(), folder_id);
  return writePackedGridFile(folder_id, packed.data(), QUARTERS_PER_GRID);
}

bool UiTilesModel::loadGridFromBinaryFile(uint16_t folder_id, std::vector<Tile>& out_grid) {
  std::vector<PackedQuarterGridV7> packed(QUARTERS_PER_GRID);
  String filePath = tileGridFile(folder_id);
  if (!readPackedGridFile(filePath, packed.data(), QUARTERS_PER_GRID)) return false;
  out_grid.clear();
  out_grid.resize(TILES_PER_GRID);
  for (size_t q = 0; q < QUARTERS_PER_GRID; ++q) {
    for (size_t ti = 0; ti < TILES_PER_QUARTER; ++ti) {
      size_t global_index = q * TILES_PER_QUARTER + ti;
      if (global_index >= TILES_PER_GRID) continue;
      unpackV7ToTile(packed[q].tiles[ti], out_grid[global_index], folder_id, global_index);
    }
  }
  return true;
}

}  // namespace uitiles
