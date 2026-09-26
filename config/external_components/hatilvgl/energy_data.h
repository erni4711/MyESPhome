#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <ArduinoJson.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace web_admin_local {

struct EnergyBucket {
  float value = 0.0f;
  bool valid = false;
  int64_t start_ms = 0;
};

struct EnergySnapshot {
  std::string statistic_id;
  std::string name;
  std::string unit;
  int8_t sign = 1;
  bool configured = false;
  bool metadata_ready = false;
  bool loading = false;
  bool stale = false;
  bool error = false;
  std::string error_message;
  float today_total = 0.0f;
  bool today_total_valid = false;
  uint32_t generation = 0;
  std::array<EnergyBucket, 24> day;
  std::array<EnergyBucket, 7> week;
};

void energy_set_visible_statistics(const std::vector<std::string> &statistic_ids);
void energy_service();
void energy_handle_ws_message(const JsonDocument &doc);
void energy_on_websocket_reconnected();

void energy_register_widget(const std::string &statistic_id,
                            lv_obj_t *value_label, lv_obj_t *unit_label,
                            int decimals, const std::string &configured_unit);
void energy_clear_widgets();
bool energy_get_snapshot(const std::string &statistic_id, EnergySnapshot &out);
void energy_request_refresh(const std::string &statistic_id, bool include_week);

// JSON fragments consumed by Web Admin. These functions are safe to call from
// the HTTP server task while the LVGL loop updates the Energy cache.
std::string energy_options_json();
std::string energy_preview_values_json();
std::string energy_preview_units_json();

}  // namespace web_admin_local
