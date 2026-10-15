// Settings tile and focused display settings overlay.
#include "tiles_lvgl.h"
#include "hatilvgl.h"
#include "../hatifonts/mdi_icons.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include "esphome/core/defines.h"
#ifdef USE_WIFI
#include <esp_wifi.h>
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace web_admin_local {

namespace {

constexpr uint32_t WIFI_REFRESH_MS = 2000;

struct WifiInfoLabels {
  lv_obj_t *status{nullptr};
  lv_obj_t *signal{nullptr};
  lv_obj_t *address{nullptr};
  lv_timer_t *timer{nullptr};
};

const char *signal_quality(int rssi) {
  if (rssi >= -50) return "excellent";
  if (rssi >= -60) return "good";
  if (rssi >= -70) return "fair";
  return "weak";
}

void update_wifi_info(WifiInfoLabels *labels) {
#ifdef USE_WIFI
  auto *wifi = esphome::wifi::global_wifi_component;
  wifi_ap_record_t ap{};
  const bool connected = wifi != nullptr && wifi->is_connected() &&
                         esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  char text[128];
  if (!connected) {
    lv_label_set_text(labels->status, "WiFi: disconnected");
    lv_obj_set_style_text_color(labels->status, lv_color_make(0xE8, 0x70, 0x70), 0);
    lv_label_set_text(labels->signal, "");
    lv_label_set_text(labels->address, "");
    return;
  }

  char ssid[33];
  const size_t ssid_length = strnlen(reinterpret_cast<const char *>(ap.ssid), 32);
  std::memcpy(ssid, ap.ssid, ssid_length);
  ssid[ssid_length] = '\0';
  std::snprintf(text, sizeof(text), "WiFi: %s (channel %u)", ssid,
                static_cast<unsigned>(ap.primary));
  lv_label_set_text(labels->status, text);
  lv_obj_set_style_text_color(labels->status, lv_color_make(0x50, 0xD8, 0x88), 0);

  const int rssi = ap.rssi;
  const int percent = rssi <= -100 ? 0 : (rssi >= -50 ? 100 : 2 * (rssi + 100));
  int8_t power_quarter_dbm = 0;
  if (esp_wifi_get_max_tx_power(&power_quarter_dbm) == ESP_OK) {
    std::snprintf(text, sizeof(text),
                  "Signal: %d dBm (%d%%, %s)  |  TX power: %.2f dBm", rssi,
                  percent, signal_quality(rssi), power_quarter_dbm / 4.0f);
  } else {
    std::snprintf(text, sizeof(text), "Signal: %d dBm (%d%%, %s)  |  TX power: n/a",
                  rssi, percent, signal_quality(rssi));
  }
  lv_label_set_text(labels->signal, text);

  char ip[esphome::network::IP_ADDRESS_BUFFER_SIZE] = "";
  for (const auto &address : wifi->get_ip_addresses()) {
    if (address.is_set() && address.is_ip4()) {
      address.str_to(ip);
      break;
    }
  }
  std::snprintf(text, sizeof(text), "IP: %s  |  BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
                ip[0] != '\0' ? ip : "-", ap.bssid[0], ap.bssid[1], ap.bssid[2],
                ap.bssid[3], ap.bssid[4], ap.bssid[5]);
  lv_label_set_text(labels->address, text);
#else
  lv_label_set_text(labels->status, "WiFi: not configured");
  lv_label_set_text(labels->signal, "");
  lv_label_set_text(labels->address, "");
#endif
}

void wifi_info_timer_cb(lv_timer_t *timer) {
  update_wifi_info(static_cast<WifiInfoLabels *>(lv_timer_get_user_data(timer)));
}

void wifi_info_delete_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
  auto *labels = static_cast<WifiInfoLabels *>(lv_event_get_user_data(event));
  if (labels->timer != nullptr) lv_timer_delete(labels->timer);
  delete labels;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t y,
                     const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  return label;
}

void settings_back_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_obj_t *button =
        static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    lv_obj_del(lv_obj_get_parent(button));
  }
}

void settings_slider_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  const intptr_t kind = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  const int value = lv_slider_get_value(slider);
  if (kind == 1) {
    settings_set_brightness(static_cast<float>(value) / 100.0f);
  } else {
    settings_set_timeout(static_cast<float>(value));
  }
}

void settings_click_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;

  lv_obj_t *tile = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_obj_t *page = lv_obj_get_parent(tile);
  lv_obj_t *overlay = lv_obj_create(page);
  lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
  lv_obj_center(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_pad_all(overlay, 16, 0);

  make_label(overlay, "Settings", 8, ui_font_for_size(22), lv_color_white());
  make_label(overlay,
             hatilvgl_is_api_connected() ? "Home Assistant: connected"
                                         : "Home Assistant: disconnected",
             48, ui_font_for_size(14),
             hatilvgl_is_api_connected() ? lv_color_make(0x50, 0xD8, 0x88)
                                         : lv_color_make(0xE8, 0x70, 0x70));

  const size_t internal_total =
      heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t psram_total =
      heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const size_t psram_free =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  char memory_text[96];
  const auto format_memory = [](char *buffer, size_t length, const char *name,
                                size_t total, size_t free) {
    const size_t used = total > free ? total - free : 0;
    const unsigned percent =
        total == 0 ? 0 : static_cast<unsigned>((used * 100U) / total);
    std::snprintf(buffer, length, "%s: %u%% used (%u / %u KiB)",
                  name, percent, static_cast<unsigned>(used / 1024U),
                  static_cast<unsigned>(total / 1024U));
  };
  format_memory(memory_text, sizeof(memory_text), "Internal RAM",
                internal_total, internal_free);
  make_label(overlay, memory_text, 84, ui_font_for_size(13), lv_color_white());
  format_memory(memory_text, sizeof(memory_text), "PSRAM",
                psram_total, psram_free);
  make_label(overlay, memory_text, 108, ui_font_for_size(13),
             lv_color_make(0x80, 0xD8, 0xFF));

  auto *wifi_labels = new WifiInfoLabels();
  wifi_labels->status = make_label(overlay, "WiFi: ...", 140, ui_font_for_size(13),
                                   lv_color_white());
  wifi_labels->signal = make_label(overlay, "", 164, ui_font_for_size(13),
                                   lv_color_white());
  wifi_labels->address = make_label(overlay, "", 188, ui_font_for_size(13),
                                    lv_color_make(0xB0, 0xB0, 0xB0));
  update_wifi_info(wifi_labels);
  wifi_labels->timer = lv_timer_create(wifi_info_timer_cb, WIFI_REFRESH_MS, wifi_labels);
  lv_obj_add_event_cb(overlay, wifi_info_delete_cb, LV_EVENT_DELETE, wifi_labels);

  make_label(overlay, "Brightness", 228, ui_font_for_size(14), lv_color_white());
  lv_obj_t *brightness = lv_slider_create(overlay);
  lv_obj_set_width(brightness, lv_pct(82));
  lv_obj_align(brightness, LV_ALIGN_TOP_MID, 0, 254);
  lv_slider_set_range(brightness, 1, 100);
  lv_slider_set_value(brightness, 100, LV_ANIM_OFF);
  lv_obj_add_event_cb(brightness, settings_slider_cb, LV_EVENT_RELEASED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(1)));

  make_label(overlay, "Screen timeout (seconds)", 302, ui_font_for_size(14),
             lv_color_white());
  lv_obj_t *timeout = lv_slider_create(overlay);
  lv_obj_set_width(timeout, lv_pct(82));
  lv_obj_align(timeout, LV_ALIGN_TOP_MID, 0, 328);
  lv_slider_set_range(timeout, 5, 600);
  lv_slider_set_value(timeout, 60, LV_ANIM_OFF);
  lv_obj_add_event_cb(timeout, settings_slider_cb, LV_EVENT_RELEASED,
                      reinterpret_cast<void *>(static_cast<intptr_t>(2)));

  lv_obj_t *back = lv_btn_create(overlay);
  lv_obj_set_size(back, 120, 42);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, "Back");
  lv_obj_center(back_label);
  lv_obj_set_style_text_font(back_label, ui_font_for_size(16), 0);
}

}  // namespace

void tile_widget_build_settings(lv_obj_t *parent, const TileData &tile) {
  const lv_color_t white = lv_color_white();
  const lv_color_t accent = lv_color_make(0x26, 0xA6, 0x9A);
  const char *title = tile.title.empty() ? "Settings" : tile.title.c_str();

  lv_obj_t *battery_icon = lv_label_create(parent);
  lv_label_set_text(battery_icon, getMdiChar("battery-outline").c_str());
  lv_obj_set_style_text_color(battery_icon, lv_color_white(), 0);
  lv_obj_set_style_text_font(battery_icon, FONT_MDI_ICONS, 0);
  lv_obj_align(battery_icon, LV_ALIGN_TOP_RIGHT, -6, 4);
  hatilvgl_register_battery_icon(battery_icon);

  lv_obj_t *title_label = lv_label_create(parent);
  lv_label_set_text(title_label, title);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title_label, LV_PCT(90));
  lv_obj_set_style_text_color(title_label, white, 0);
  lv_obj_set_style_text_font(title_label, ui_font_for_size(16), 0);
  lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 18);

  lv_obj_t *battery = make_label(parent, "--%", 0,
                                 ui_font_for_size(13), white);
  lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, 6, 12);
  hatilvgl_register_battery_label(battery);

  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, settings_click_cb, LV_EVENT_CLICKED, nullptr);
}

}  // namespace web_admin_local
