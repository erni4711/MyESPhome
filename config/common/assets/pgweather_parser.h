#pragma once
#include <string>
#include <cstdlib>
#include <cerrno>

#include "esphome.h"
#include "esphome/components/lvgl/lvgl_esphome.h"

// Inline, static implementation to ensure the parser is available
// to the single translation unit that includes this header.
static inline void safe_lv_label_set_text(lv_obj_t* label, const char* txt) {
  if (!label) return;
  if (!txt) txt = "";
  lv_label_set_text(label, txt);
}

static const char* mdi_clear_night = "\U0000EA62";
static const char* mdi_cloudy = "\U0000E2BD";
static const char* mdi_fog = "\U0000E818";
static const char* mdi_lightning_rainy = "\U0000F61E";
static const char* mdi_pouring = "\U0000F61F";
static const char* mdi_rainy = "\U0000F176";
static const char* mdi_snowy = "\U0000E80F";
static const char* mdi_snowy_night = "\U0000E2CD";
static const char* mdi_snowy_rainy = "\U0000F61D";
static const char* mdi_sunny = "\U0000E81A";
static const char* mdi_unknown = "\U0000E6A5";

static inline std::string icon_code_to_glyph(const std::string& icon_code) {
  if (icon_code == "01d")
    return std::string(mdi_sunny);
  else if (icon_code == "01n")
    return std::string(mdi_clear_night);
  else if (icon_code == "02d" || icon_code == "02n")
    return std::string(mdi_cloudy);
  else if (icon_code == "03d" || icon_code == "03n" || icon_code == "04d" ||
           icon_code == "04n")
    return std::string(mdi_cloudy);
  else if (icon_code == "09d" || icon_code == "09n")
    return std::string(mdi_pouring);
  else if (icon_code == "10d" || icon_code == "10n")
    return std::string(mdi_rainy);
  else if (icon_code == "11d" || icon_code == "11n")
    return std::string(mdi_lightning_rainy);
  else if (icon_code == "13d" || icon_code == "13n")
    return std::string(mdi_snowy);
  else if (icon_code == "50d" || icon_code == "50n")
    return std::string(mdi_fog);
  return std::string(mdi_unknown);
}

static inline void pgweather_daily_parse_attrs(
    const std::string& json, lv_obj_t* day_label, lv_obj_t* description_label,
    lv_obj_t* icon_lbl, lv_obj_t* temp_hi_lbl, lv_obj_t* temp_lo_lbl,
    lv_obj_t* sunrise_lbl, lv_obj_t* sunset_lbl) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    ESP_LOGW("pgweather", "owm_daily deserializeJson failed: %s", err.c_str());
    return;
  }

  float hi = 0.0f, lo = 0.0f;
  
  hi = doc["temp_max"] | 0.0f;
  lo = doc["temp_min"] | 0.0f;
 
  char hibuf[16];
  snprintf(hibuf, sizeof(hibuf), "%.1f°", hi);
  char lobuf[16];
  snprintf(lobuf, sizeof(lobuf), "%.1f°", lo);
  safe_lv_label_set_text(temp_hi_lbl,
                         hibuf);  // Show hi temp only for daily summary
  safe_lv_label_set_text(temp_lo_lbl,
                         lobuf);  // Show lo temp only for daily summary

  if (doc["weather"].is<JsonArray>() && doc["weather"].size() > 0) {
    JsonObject w = doc["weather"][0].as<JsonObject>();
    const std::string desc = w["description"] | "";
    const std::string icon = w["icon"] | "";
    safe_lv_label_set_text(description_label, desc.c_str());
    safe_lv_label_set_text(icon_lbl, icon_code_to_glyph(icon).c_str());
  }

  // Sunrise / sunset handling: format timestamps as HH:MM and prefix with glyphs
  if (sunrise_lbl) {
    long sr = doc["sunrise"] | 0L;
    if (sr) {
      time_t t = (time_t)sr;
      struct tm* lt = localtime(&t);
      char timestr[16] = {0};
      if (lt) strftime(timestr, sizeof(timestr), "%H:%M", lt);
      std::string s = std::string(mdi_sunny) + " ";
      s += timestr;
      safe_lv_label_set_text(sunrise_lbl, s.c_str());
    } else {
      safe_lv_label_set_text(sunrise_lbl, "");
    }
  }

  if (sunset_lbl) {
    long ss = doc["sunset"] | 0L;
    if (ss) {
      time_t t = (time_t)ss;
      struct tm* lt = localtime(&t);
      char timestr[16] = {0};
      if (lt) strftime(timestr, sizeof(timestr), "%H:%M", lt);
      std::string s = std::string(mdi_clear_night) + " ";
      s += timestr;
      safe_lv_label_set_text(sunset_lbl, s.c_str());
    } else {
      safe_lv_label_set_text(sunset_lbl, "");
    }
  }
}

// Split: parse state (timestamp) separately from attributes (json payload)
static inline void pgweather_daily_parse_state(const std::string& state,
                                              lv_obj_t* day_label) {
  if (state.empty() || !day_label) return;
  // try parse as integer timestamp
  long dt = 0L;
  if (!state.empty()) {
    errno = 0;
    char* endptr = nullptr;
    long v = strtol(state.c_str(), &endptr, 10);
    if (endptr != state.c_str() && *endptr == '\0' && errno == 0) {
      dt = v;
    } else {
      dt = 0L;
    }
  }
  char daybuf[16] = {0};
  if (dt) {
    time_t t = (time_t)dt;
    struct tm* lt = localtime(&t);
    if (lt) strftime(daybuf, sizeof(daybuf), "%a", lt);
  }
  safe_lv_label_set_text(day_label, daybuf);
}


static inline void pgweather_hourly_parse_attrs(const std::string& json,
                                           lv_obj_t* time_label,
                                           lv_obj_t* icon_lbl,
                                           lv_obj_t* temp_lbl,
                                           lv_obj_t* pop_lbl) {
  if (json.empty()) return;

  JsonDocument doc;
  auto err = deserializeJson(doc, json.c_str());
  if (err) {
    ESP_LOGW("pgweather", "owm_hourly_parse failed: %s", err.c_str());
    return;
  }

  std::string icon_code = "";
  if (doc["weather"].is<JsonArray>() &&
      doc["weather"][0]["icon"].is<const char*>())
    icon_code = doc["weather"][0]["icon"].as<const char*>();
  else if (doc["icon"].is<const char*>())
    icon_code = doc["icon"].as<const char*>();

  std::string glyph = icon_code_to_glyph(icon_code);

  safe_lv_label_set_text(icon_lbl, glyph.c_str());

  if (doc["temp"].is<double>() || doc["temp"].is<long>()) {
    double t = doc["temp"].as<double>();
    char tb[18];
    snprintf(tb, sizeof(tb), "%.1f°", t);
    safe_lv_label_set_text(temp_lbl, tb);
  } else {
    safe_lv_label_set_text(temp_lbl, "");
  }

  if (doc["pop"].is<double>() || doc["pop"].is<long>()) {
    double p = doc["pop"].as<double>();
    int pi = (int)(p * 100.0 + 0.5);
    char pb[8];
    snprintf(pb, sizeof(pb), "%d%%", pi);
    safe_lv_label_set_text(pop_lbl, pb);
  } else {
    safe_lv_label_set_text(pop_lbl, "");
  }
}

static inline void pgweather_hourly_parse_state(const std::string& state,
                                               lv_obj_t* time_label) {
  if (state.empty() || !time_label) return;
  long dt = 0L;
  if (!state.empty()) {
    errno = 0;
    char* endptr = nullptr;
    long v = strtol(state.c_str(), &endptr, 10);
    if (endptr != state.c_str() && *endptr == '\0' && errno == 0) {
      dt = v;
    } else {
      dt = 0L;
    }
  }
  char tsbuf[16] = {0};
  if (dt) {
    time_t t = (time_t)dt;
    struct tm* lt = localtime(&t);
    if (lt) strftime(tsbuf, sizeof(tsbuf), "%H:%M", lt);
  }
  safe_lv_label_set_text(time_label, tsbuf);
}

