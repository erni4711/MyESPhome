#include "energy_data.h"

#include "ha_ws_client.h"
#include "tiles_lvgl.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace web_admin_local {
namespace {

constexpr uint32_t kDayRefreshMs = 60UL * 1000UL;
constexpr uint32_t kWeekRefreshMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kRequestTimeoutMs = 15UL * 1000UL;
constexpr uint32_t kRetryMs = 3UL * 1000UL;

enum class RequestKind : uint8_t {
  NONE,
  PREFERENCES,
  METADATA,
  DAY,
  WEEK,
};

struct PendingRequest {
  uint32_t id = 0;
  uint32_t sent_ms = 0;
  RequestKind kind = RequestKind::NONE;
  std::vector<std::string> statistic_ids;
};

struct EnergyRecord {
  EnergySnapshot snapshot;
  std::string category;
  bool synthetic = false;
  uint32_t day_updated_ms = 0;
  uint32_t week_updated_ms = 0;
};

struct EnergyWidgetBinding {
  lv_obj_t *value_label = nullptr;
  lv_obj_t *unit_label = nullptr;
  int decimals = 1;
  std::string configured_unit;
};

std::unordered_map<std::string, EnergyRecord> g_records;
std::unordered_map<std::string, std::vector<EnergyWidgetBinding>> g_widgets;
std::vector<std::string> g_visible_ids;
PendingRequest g_pending;
bool g_preferences_ready = false;
bool g_metadata_requested = false;
bool g_force_refresh = true;
bool g_include_week = false;
uint32_t g_retry_after_ms = 0;
int g_cached_local_day = -1;

SemaphoreHandle_t snapshot_mutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

class SnapshotLock {
 public:
  SnapshotLock() { xSemaphoreTake(snapshot_mutex(), portMAX_DELAY); }
  ~SnapshotLock() { xSemaphoreGive(snapshot_mutex()); }
};

bool elapsed(uint32_t now, uint32_t then, uint32_t interval) {
  return then == 0 || static_cast<uint32_t>(now - then) >= interval;
}

uint32_t now_ms() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

std::string json_escape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
        break;
    }
  }
  return out;
}

std::string humanize_id(const std::string &id) {
  size_t dot = id.find('.');
  std::string text = dot == std::string::npos ? id : id.substr(dot + 1);
  bool upper = true;
  for (char &c : text) {
    if (c == '_' || c == '-') {
      c = ' ';
      upper = true;
    } else if (upper) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      upper = false;
    }
  }
  return text;
}

bool valid_wall_clock(struct tm *local_now = nullptr) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm value {};
  localtime_r(&now, &value);
  if (local_now) *local_now = value;
  return true;
}

std::string iso_local(time_t value) {
  struct tm local {};
  localtime_r(&value, &local);
  char buffer[40];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &local);
  return buffer;
}

bool period_bounds(bool week, std::string &start, std::string &end,
                   time_t *start_epoch = nullptr) {
  struct tm local_now {};
  if (!valid_wall_clock(&local_now)) return false;
  local_now.tm_hour = 0;
  local_now.tm_min = 0;
  local_now.tm_sec = 0;
  local_now.tm_isdst = -1;
  time_t midnight = mktime(&local_now);
  if (week) {
    local_now.tm_mday -= 6;
    midnight = mktime(&local_now);
  }
  struct tm local_end {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local_end);
  local_end.tm_hour = 0;
  local_end.tm_min = 0;
  local_end.tm_sec = 0;
  local_end.tm_mday += 1;
  local_end.tm_isdst = -1;
  const time_t tomorrow = mktime(&local_end);
  start = iso_local(midnight);
  end = iso_local(tomorrow);
  if (start_epoch) *start_epoch = midnight;
  return true;
}

void set_error_for_visible(const char *message) {
  SnapshotLock lock;
  for (const auto &id : g_visible_ids) {
    auto &snapshot = g_records[id].snapshot;
    snapshot.statistic_id = id;
    snapshot.loading = false;
    snapshot.error = true;
    snapshot.error_message = message ? message : "Unavailable";
    ++snapshot.generation;
  }
}

void format_total(char *buffer, size_t size, const EnergySnapshot &snapshot,
                  int decimals) {
  if (!snapshot.configured) {
    std::snprintf(buffer, size, "Unavailable");
  } else if (snapshot.error) {
    std::snprintf(buffer, size, "Unavailable");
  } else if (!snapshot.today_total_valid) {
    std::snprintf(buffer, size, "%s", snapshot.loading ? "Loading..." : "--");
  } else {
    const int digits = std::max(0, std::min(6, decimals < 0 ? 1 : decimals));
    std::snprintf(buffer, size, "%.*f", digits, snapshot.today_total);
  }
}

void update_widgets(const std::string &id) {
  const auto record_it = g_records.find(id);
  const auto widget_it = g_widgets.find(id);
  if (record_it == g_records.end() || widget_it == g_widgets.end()) return;
  const EnergySnapshot &snapshot = record_it->second.snapshot;
  for (const auto &binding : widget_it->second) {
    char value[48];
    format_total(value, sizeof(value), snapshot, binding.decimals);
    if (binding.value_label) lv_label_set_text(binding.value_label, value);
    if (binding.unit_label) {
      const std::string &unit =
          binding.configured_unit.empty() ? snapshot.unit : binding.configured_unit;
      lv_label_set_text(binding.unit_label,
                        snapshot.today_total_valid ? unit.c_str() : "");
    }
  }
}

void update_all_widgets() {
  for (const auto &entry : g_widgets) update_widgets(entry.first);
}

bool is_electricity_category(const std::string &category) {
  return category == "solar" || category == "grid" ||
         category == "battery";
}

void add_source(const std::string &id, int8_t sign,
                const std::string &category) {
  if (id.empty()) return;
  auto &record = g_records[id];
  auto &snapshot = record.snapshot;
  snapshot.statistic_id = id;
  snapshot.configured = true;
  snapshot.sign = sign;
  record.category = category;
  record.synthetic = false;
  if (snapshot.name.empty()) snapshot.name = humanize_id(id);
}

void add_statistic(JsonObjectConst source, const char *key, int8_t sign,
                   const std::string &category) {
  const char *id = source[key] | "";
  if (id[0]) add_source(id, sign, category);
}

void collect_preference_sources(JsonVariantConst preferences) {
  JsonArrayConst sources = preferences["energy_sources"].as<JsonArrayConst>();
  for (JsonObjectConst source : sources) {
    const std::string category = source["type"] | "";
    if (category == "grid") {
      add_statistic(source, "stat_energy_from", 1, category);
      add_statistic(source, "stat_energy_to", -1, category);
      for (JsonObjectConst flow : source["flow_from"].as<JsonArrayConst>())
        add_statistic(flow, "stat_energy_from", 1, category);
      for (JsonObjectConst flow : source["flow_to"].as<JsonArrayConst>())
        add_statistic(flow, "stat_energy_to", -1, category);
    } else if (category == "battery") {
      add_statistic(source, "stat_energy_from", 1, category);
      add_statistic(source, "stat_energy_to", -1, category);
    } else if (category == "solar" || category == "gas" ||
               category == "water") {
      add_statistic(source, "stat_energy_from", 1, category);
    }
  }

  for (JsonObjectConst device :
       preferences["device_consumption"].as<JsonArrayConst>())
    add_statistic(device, "stat_consumption", 1, "device");
  for (JsonObjectConst device :
       preferences["device_consumption_water"].as<JsonArrayConst>())
    add_statistic(device, "stat_consumption", 1, "device_water");

  const bool has_electricity = std::any_of(
      g_records.begin(), g_records.end(), [](const auto &entry) {
        return entry.second.snapshot.configured &&
               is_electricity_category(entry.second.category);
      });
  if (has_electricity) {
    auto &record = g_records["consumption_total"];
    record.category = "consumption";
    record.synthetic = true;
    auto &snapshot = record.snapshot;
    snapshot.statistic_id = "consumption_total";
    snapshot.name = "House consumed";
    snapshot.unit = "kWh";
    snapshot.sign = 1;
    snapshot.configured = true;
    snapshot.metadata_ready = true;
    snapshot.error = false;
    snapshot.error_message.clear();
    ++snapshot.generation;
  }
}

std::vector<std::string> configured_ids() {
  std::vector<std::string> ids;
  ids.reserve(g_records.size());
  for (const auto &entry : g_records) {
    if (entry.second.snapshot.configured && !entry.second.synthetic)
      ids.push_back(entry.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::string ids_json(const std::vector<std::string> &ids) {
  std::string out = "[";
  bool first = true;
  for (const auto &id : ids) {
    if (!first) out += ',';
    first = false;
    out += '"' + json_escape(id) + '"';
  }
  out += ']';
  return out;
}

bool begin_request(RequestKind kind, const std::string &fields,
                   const std::vector<std::string> &statistic_ids = {}) {
  const uint32_t id = ha_ws_client_send_request(fields);
  if (id == 0) return false;
  g_pending.id = id;
  g_pending.sent_ms = now_ms();
  g_pending.kind = kind;
  g_pending.statistic_ids = statistic_ids;
  return true;
}

bool request_preferences() {
  return begin_request(RequestKind::PREFERENCES,
                       "\"type\":\"energy/get_prefs\"");
}

bool request_metadata() {
  const auto ids = configured_ids();
  if (ids.empty()) {
    g_metadata_requested = true;
    return true;
  }
  return begin_request(
      RequestKind::METADATA,
      "\"type\":\"recorder/get_statistics_metadata\",\"statistic_ids\":" +
          ids_json(ids));
}

bool request_period(bool week) {
  std::vector<std::string> ids;
  bool consumption_requested = false;
  for (const auto &id : g_visible_ids) {
    const auto it = g_records.find(id);
    if (it == g_records.end()) continue;
    if (it->second.synthetic && id == "consumption_total") {
      consumption_requested = true;
    } else if (it->second.snapshot.configured &&
               it->second.snapshot.metadata_ready) {
      ids.push_back(id);
    }
  }
  if (consumption_requested) {
    for (const auto &entry : g_records) {
      if (!entry.second.synthetic && entry.second.snapshot.configured &&
          entry.second.snapshot.metadata_ready &&
          is_electricity_category(entry.second.category) &&
          std::find(ids.begin(), ids.end(), entry.first) == ids.end()) {
        ids.push_back(entry.first);
      }
    }
  }
  if (ids.empty()) {
    g_force_refresh = false;
    g_include_week = false;
    return false;
  }
  std::string start;
  std::string end;
  if (!period_bounds(week, start, end)) {
    set_error_for_visible("Waiting for local time");
    update_all_widgets();
    return false;
  }
  {
    SnapshotLock lock;
    for (const auto &id : ids) {
      auto &snapshot = g_records[id].snapshot;
      snapshot.loading = true;
      snapshot.error = false;
      snapshot.error_message.clear();
      ++snapshot.generation;
    }
    if (consumption_requested) {
      auto &snapshot = g_records["consumption_total"].snapshot;
      snapshot.loading = true;
      snapshot.error = false;
      snapshot.error_message.clear();
      ++snapshot.generation;
    }
  }
  const std::string fields =
      "\"type\":\"recorder/statistics_during_period\","
      "\"start_time\":\"" + start + "\",\"end_time\":\"" + end +
      "\",\"statistic_ids\":" + ids_json(ids) +
      ",\"period\":\"" + (week ? "day" : "hour") +
      "\",\"types\":[\"change\"]";
  return begin_request(week ? RequestKind::WEEK : RequestKind::DAY, fields,
                       ids);
}

int day_offset(time_t start, time_t period_start) {
  struct tm current {};
  struct tm base {};
  localtime_r(&start, &current);
  localtime_r(&period_start, &base);
  current.tm_hour = base.tm_hour = 12;
  current.tm_min = base.tm_min = 0;
  current.tm_sec = base.tm_sec = 0;
  current.tm_isdst = base.tm_isdst = -1;
  return static_cast<int>((mktime(&current) - mktime(&base)) / (24 * 60 * 60));
}

void derive_consumption_total(bool week, uint32_t response_ms) {
  auto record_it = g_records.find("consumption_total");
  if (record_it == g_records.end() || !record_it->second.synthetic) return;
  EnergyRecord &record = record_it->second;
  EnergySnapshot &snapshot = record.snapshot;
  if (week)
    snapshot.week = {};
  else {
    snapshot.day = {};
    snapshot.today_total = 0.0f;
    snapshot.today_total_valid = false;
  }

  const int slots = week ? 7 : 24;
  bool any_value = false;
  for (int slot = 0; slot < slots; ++slot) {
    float total = 0.0f;
    bool valid = false;
    int64_t start_ms = 0;
    for (const auto &entry : g_records) {
      if (entry.second.synthetic ||
          !entry.second.snapshot.configured ||
          !is_electricity_category(entry.second.category))
        continue;
      const EnergyBucket &source =
          week ? entry.second.snapshot.week[slot]
               : entry.second.snapshot.day[slot];
      if (!source.valid) continue;
      total += source.value;
      valid = true;
      if (start_ms == 0) start_ms = source.start_ms;
    }
    if (!valid) continue;
    EnergyBucket &target = week ? snapshot.week[slot] : snapshot.day[slot];
    target.value = total;
    target.valid = true;
    target.start_ms = start_ms;
    any_value = true;
    if (!week) {
      snapshot.today_total += total;
      snapshot.today_total_valid = true;
    }
  }
  snapshot.loading = false;
  snapshot.error = !any_value;
  snapshot.error_message =
      any_value ? "" : "No electricity statistics data";
  ++snapshot.generation;
  if (week)
    record.week_updated_ms = response_ms;
  else
    record.day_updated_ms = response_ms;
}

void parse_period_result(JsonVariantConst result, bool week,
                         const std::vector<std::string> &requested_ids) {
  std::string ignored_start;
  std::string ignored_end;
  time_t period_start = 0;
  if (!period_bounds(week, ignored_start, ignored_end, &period_start)) return;

  SnapshotLock lock;
  const uint32_t response_ms = now_ms();
  for (const auto &id : requested_ids) {
    auto record_it = g_records.find(id);
    if (record_it == g_records.end()) continue;
    EnergyRecord &record = record_it->second;
    EnergySnapshot &snapshot = record.snapshot;
    if (week) {
      snapshot.week = {};
    } else {
      snapshot.day = {};
      snapshot.today_total = 0.0f;
      snapshot.today_total_valid = false;
    }

    JsonVariantConst rows_value = result[id.c_str()];
    if (!rows_value.is<JsonArrayConst>()) {
      snapshot.loading = false;
      if (!week) {
        snapshot.error = true;
        snapshot.error_message = "Statistic unavailable";
      }
      if (week)
        record.week_updated_ms = response_ms;
      else
        record.day_updated_ms = response_ms;
      ++snapshot.generation;
      continue;
    }

    for (JsonObjectConst row : rows_value.as<JsonArrayConst>()) {
      JsonVariantConst change = row["change"];
      if (change.isNull()) continue;
      const int64_t start_ms = row["start"] | static_cast<int64_t>(0);
      if (start_ms <= 0) continue;
      const time_t start_time = static_cast<time_t>(start_ms / 1000);
      int slot = 0;
      if (week) {
        slot = day_offset(start_time, period_start);
        if (slot < 0 || slot >= 7) continue;
      } else {
        struct tm local {};
        localtime_r(&start_time, &local);
        slot = local.tm_hour;
        if (slot < 0 || slot >= 24) continue;
      }
      EnergyBucket &bucket = week ? snapshot.week[slot] : snapshot.day[slot];
      bucket.value = change.as<float>() * snapshot.sign;
      bucket.valid = true;
      bucket.start_ms = start_ms;
      if (!week) {
        snapshot.today_total += bucket.value;
        snapshot.today_total_valid = true;
      }
    }
    snapshot.loading = false;
    if (!week) {
      snapshot.error = false;
      snapshot.error_message.clear();
    }
    ++snapshot.generation;
    if (week)
      record.week_updated_ms = response_ms;
    else
      record.day_updated_ms = response_ms;
  }
  derive_consumption_total(week, response_ms);
}

void parse_metadata(JsonVariantConst result) {
  if (!result.is<JsonArrayConst>()) return;
  SnapshotLock lock;
  for (auto &entry : g_records) {
    if (entry.second.snapshot.configured && !entry.second.synthetic)
      entry.second.snapshot.metadata_ready = false;
  }
  for (JsonObjectConst item : result.as<JsonArrayConst>()) {
    const char *id = item["statistic_id"] | "";
    if (!id[0]) continue;
    auto it = g_records.find(id);
    if (it == g_records.end()) continue;
    EnergySnapshot &snapshot = it->second.snapshot;
    const char *name = item["name"] | "";
    const char *unit = item["statistics_unit_of_measurement"] | "";
    if (name[0]) snapshot.name = name;
    if (unit[0]) snapshot.unit = unit;
    snapshot.metadata_ready = item["has_sum"] | false;
  }
  for (const auto &id : g_visible_ids) {
    auto it = g_records.find(id);
    if (it == g_records.end()) continue;
    EnergySnapshot &snapshot = it->second.snapshot;
    if (snapshot.configured && !snapshot.metadata_ready &&
        !it->second.synthetic) {
      snapshot.loading = false;
      snapshot.error = true;
      snapshot.error_message = "Statistic has no sum data";
      ++snapshot.generation;
    }
  }
}

}  // namespace

void energy_set_visible_statistics(
    const std::vector<std::string> &statistic_ids) {
  SnapshotLock lock;
  g_visible_ids.clear();
  for (const auto &id : statistic_ids) {
    if (id.empty() ||
        std::find(g_visible_ids.begin(), g_visible_ids.end(), id) !=
            g_visible_ids.end())
      continue;
    g_visible_ids.push_back(id);
    auto &snapshot = g_records[id].snapshot;
    snapshot.statistic_id = id;
    if (snapshot.name.empty()) snapshot.name = humanize_id(id);
  }
  g_force_refresh = true;
}

void energy_on_websocket_reconnected() {
  g_preferences_ready = false;
  g_metadata_requested = false;
  g_pending = {};
  g_force_refresh = true;
  g_retry_after_ms = 0;
}

void energy_service() {
  if (!ha_ws_client_is_authenticated()) return;
  const uint32_t now = now_ms();
  if (g_pending.id != 0) {
    if (!elapsed(now, g_pending.sent_ms, kRequestTimeoutMs)) return;
    g_pending = {};
    g_retry_after_ms = now;
    set_error_for_visible("Home Assistant request timed out");
    update_all_widgets();
    return;
  }
  if (g_retry_after_ms != 0 && !elapsed(now, g_retry_after_ms, kRetryMs)) return;

  struct tm local_now {};
  if (valid_wall_clock(&local_now) && local_now.tm_yday != g_cached_local_day) {
    g_cached_local_day = local_now.tm_yday;
    g_force_refresh = true;
  }

  if (!g_preferences_ready) {
    if (!request_preferences()) g_retry_after_ms = now;
    return;
  }
  if (!g_metadata_requested) {
    if (request_metadata()) g_metadata_requested = true;
    else g_retry_after_ms = now;
    return;
  }

  bool day_due = g_force_refresh;
  bool week_due = g_force_refresh || g_include_week;
  for (const auto &id : g_visible_ids) {
    auto it = g_records.find(id);
    if (it == g_records.end() || !it->second.snapshot.configured ||
        !it->second.snapshot.metadata_ready)
      continue;
    day_due = day_due || elapsed(now, it->second.day_updated_ms, kDayRefreshMs);
    week_due =
        week_due || elapsed(now, it->second.week_updated_ms, kWeekRefreshMs);
  }
  if (day_due && request_period(false)) {
    g_force_refresh = false;
    return;
  }
  if (week_due && request_period(true)) {
    g_include_week = false;
  }
}

void energy_handle_ws_message(const JsonDocument &doc) {
  if (doc["type"] != "result") return;
  const uint32_t id = doc["id"] | 0;
  if (id == 0 || id != g_pending.id) return;
  const RequestKind kind = g_pending.kind;
  const std::vector<std::string> requested_ids = g_pending.statistic_ids;
  g_pending = {};
  const bool success = doc["success"] | false;
  if (!success) {
    set_error_for_visible(doc["error"]["message"] | "Home Assistant request failed");
    update_all_widgets();
    if (kind == RequestKind::PREFERENCES) g_preferences_ready = false;
    if (kind == RequestKind::METADATA) g_metadata_requested = false;
    if (kind == RequestKind::DAY || kind == RequestKind::WEEK)
      g_force_refresh = true;
    g_retry_after_ms = now_ms();
    return;
  }
  JsonVariantConst result = doc["result"];
  switch (kind) {
    case RequestKind::PREFERENCES: {
      {
        SnapshotLock lock;
        for (auto &entry : g_records) {
          entry.second.snapshot.configured = false;
          entry.second.category.clear();
          entry.second.synthetic = false;
        }
        collect_preference_sources(result);
      }
      g_preferences_ready = true;
      g_metadata_requested = false;
      g_force_refresh = true;
      break;
    }
    case RequestKind::METADATA:
      parse_metadata(result);
      break;
    case RequestKind::DAY:
      parse_period_result(result, false, requested_ids);
      break;
    case RequestKind::WEEK:
      parse_period_result(result, true, requested_ids);
      break;
    default:
      break;
  }
  update_all_widgets();
}

void energy_register_widget(const std::string &statistic_id,
                            lv_obj_t *value_label, lv_obj_t *unit_label,
                            int decimals,
                            const std::string &configured_unit) {
  if (statistic_id.empty()) return;
  EnergyWidgetBinding binding;
  binding.value_label = value_label;
  binding.unit_label = unit_label;
  binding.decimals = decimals;
  binding.configured_unit = configured_unit;
  g_widgets[statistic_id].push_back(binding);
  update_widgets(statistic_id);
}

void energy_clear_widgets() { g_widgets.clear(); }

bool energy_get_snapshot(const std::string &statistic_id, EnergySnapshot &out) {
  SnapshotLock lock;
  auto it = g_records.find(statistic_id);
  if (it == g_records.end()) return false;
  out = it->second.snapshot;
  const uint32_t now = now_ms();
  out.stale = elapsed(now, it->second.day_updated_ms, 2 * kDayRefreshMs);
  return true;
}

void energy_request_refresh(const std::string &statistic_id,
                            bool include_week) {
  if (!statistic_id.empty() &&
      std::find(g_visible_ids.begin(), g_visible_ids.end(), statistic_id) ==
          g_visible_ids.end()) {
    g_visible_ids.push_back(statistic_id);
  }
  g_force_refresh = true;
  g_include_week = g_include_week || include_week;
}

std::string energy_options_json() {
  SnapshotLock lock;
  std::vector<const EnergySnapshot *> sources;
  for (const auto &entry : g_records) {
    if (entry.second.snapshot.configured &&
        entry.second.snapshot.metadata_ready)
      sources.push_back(&entry.second.snapshot);
  }
  std::sort(sources.begin(), sources.end(),
            [](const EnergySnapshot *a, const EnergySnapshot *b) {
              return a->name < b->name;
            });
  std::string json = "[";
  bool first = true;
  for (const auto *source : sources) {
    if (!first) json += ',';
    first = false;
    std::string label = source->name.empty()
                            ? humanize_id(source->statistic_id)
                            : source->name;
    if (!source->unit.empty()) label += " (" + source->unit + ")";
    label += " - " + source->statistic_id;
    json += "{\"v\":\"" + json_escape(source->statistic_id) +
            "\",\"t\":\"" + json_escape(label) + "\"}";
  }
  json += ']';
  return json;
}

std::string energy_preview_values_json() {
  SnapshotLock lock;
  std::string json = "{";
  bool first = true;
  for (const auto &entry : g_records) {
    const auto &snapshot = entry.second.snapshot;
    if (!snapshot.today_total_valid) continue;
    if (!first) json += ',';
    first = false;
    char value[32];
    std::snprintf(value, sizeof(value), "%.6g", snapshot.today_total);
    json += '"' + json_escape(entry.first) + "\":" + value;
  }
  json += '}';
  return json;
}

std::string energy_preview_units_json() {
  SnapshotLock lock;
  std::string json = "{";
  bool first = true;
  for (const auto &entry : g_records) {
    const auto &snapshot = entry.second.snapshot;
    if (snapshot.unit.empty()) continue;
    if (!first) json += ',';
    first = false;
    json += '"' + json_escape(entry.first) + "\":\"" +
            json_escape(snapshot.unit) + '"';
  }
  json += '}';
  return json;
}

}  // namespace web_admin_local
