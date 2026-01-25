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


static const char* mdi_clear_night = "\U0000F34F";
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
static const char* mdi_windy = "\U0000EC0C";  
static const char* mdi_wb_twilight = "\U0000E1C6";  
static const char* mdi_exceptional = "\U0000F157";

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

static inline std::string wind_deg_to_dir(double deg) {
  // German-style abbreviations: N, NNO, NO, ONO, O, OSO, SO, SSO, S, SSW, SW, WSW, W, WNW, NW, NNW
  static const char* dirs[] = {"N","NNO","NO","ONO","O","OSO","SO","SSO","S","SSW","SW","WSW","W","WNW","NW","NNW"};
  if (std::isnan(deg)) return std::string("");
  // Normalize
  while (deg < 0) deg += 360.0;
  while (deg >= 360.0) deg -= 360.0;
  int idx = (int)((deg + 11.25) / 22.5) % 16;
  return std::string(dirs[idx]);
}

static inline void pgweather_daily_parse_attrs(
  const std::string& json, lv_obj_t* day_label, lv_obj_t* description_label,
  lv_obj_t* icon_lbl, lv_obj_t* temp_hi_lbl, lv_obj_t* temp_lo_lbl,
  lv_obj_t* sunrise_lbl, lv_obj_t* sunset_lbl, lv_obj_t* rain_lbl, lv_obj_t* snow_lbl, lv_obj_t* wind_lbl) {
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

  // Wind: show speed and compass direction if available
  if (wind_lbl) {
    if (doc["wind_speed"].is<double>() || doc["wind_speed"].is<int>()) {
      double ws = doc["wind_speed"].as<double>();
      std::string dir = "";
      if (doc["wind_deg"].is<double>() || doc["wind_deg"].is<int>()) {
        double deg = doc["wind_deg"].as<double>();
        dir = wind_deg_to_dir(deg);
      }
      char wb[64];
      if (!dir.empty())
        snprintf(wb, sizeof(wb), "%.1f㎧%s", ws, dir.c_str());
      else
        snprintf(wb, sizeof(wb), "%.1f㎧", ws);
      std::string s = std::string(mdi_windy) + " ";
      s += wb;

      safe_lv_label_set_text(wind_lbl, s.c_str());
    } else {
      safe_lv_label_set_text(wind_lbl, "");
    }
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
      std::string s = std::string(mdi_wb_twilight) + " ";
      s += timestr;
      safe_lv_label_set_text(sunset_lbl, s.c_str());
    } else {
      safe_lv_label_set_text(sunset_lbl, "");
    }
  }
  // Rain: show amount (mm) if available
  if (rain_lbl) {
    double rain_amt = 0.0;
    bool has_rain = false;
    if (doc["rain"].is<JsonObject>()) {
      if (doc["rain"]["1h"].is<double>() || doc["rain"]["1h"].is<int>()) {
        rain_amt = doc["rain"]["1h"].as<double>();
        has_rain = true;
      } else if (doc["rain"]["3h"].is<double>() || doc["rain"]["3h"].is<int>()) {
        rain_amt = doc["rain"]["3h"].as<double>();
        has_rain = true;
      }
    } else if (doc["rain"].is<double>() || doc["rain"].is<int>()) {
      rain_amt = doc["rain"].as<double>();
      has_rain = true;
    }

    if (has_rain && rain_amt > 0.0) {
      char rb[32];
      snprintf(rb, sizeof(rb), "%.1fmm", rain_amt);
      std::string s = std::string(mdi_rainy);
      s += rb;
      safe_lv_label_set_text(rain_lbl, s.c_str());
    } else {
      safe_lv_label_set_text(rain_lbl, "");
    }
  }
  // Snow: show amount (mm) if available
  if (snow_lbl) {
    double snow_amt = 0.0;
    bool has_snow = false;
    if (doc["snow"].is<JsonObject>()) {
      if (doc["snow"]["1h"].is<double>() || doc["snow"]["1h"].is<int>()) {
        snow_amt = doc["snow"]["1h"].as<double>();
        has_snow = true;
      } else if (doc["snow"]["3h"].is<double>() || doc["snow"]["3h"].is<int>()) {
        snow_amt = doc["snow"]["3h"].as<double>();
        has_snow = true;
      }
    } else if (doc["snow"].is<double>() || doc["snow"].is<int>()) {
      snow_amt = doc["snow"].as<double>();
      has_snow = true;
    }
    
    if (has_snow && snow_amt > 0.0) {
      char sb[32];
      snprintf(sb, sizeof(sb), "%.1fmm", snow_amt);
      std::string s = std::string(mdi_snowy);
      s += sb;
      safe_lv_label_set_text(snow_lbl, s.c_str());
    } else {
      safe_lv_label_set_text(snow_lbl, "");
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
    if (lt) {    
      switch (lt->tm_wday) {
        case 0: strftime(daybuf, sizeof(daybuf), "Son", lt); break;
        case 1: strftime(daybuf, sizeof(daybuf), "Mon", lt); break;
        case 2: strftime(daybuf, sizeof(daybuf), "Die", lt); break;
        case 3: strftime(daybuf, sizeof(daybuf), "Mit", lt); break;
        case 4: strftime(daybuf, sizeof(daybuf), "Don", lt); break;
        case 5: strftime(daybuf, sizeof(daybuf), "Fre", lt); break;
        case 6: strftime(daybuf, sizeof(daybuf), "Sam", lt); break;
        default: break;
      }
    }
  }
  safe_lv_label_set_text(day_label, daybuf);
}


static inline void pgweather_hourly_parse_attrs(const std::string& json,
                                           lv_obj_t* time_label,
                                           lv_obj_t* icon_lbl,
                                           lv_obj_t* temp_lbl,
                                           lv_obj_t* pop_lbl,
                                           lv_obj_t* rain_lbl,
                                           lv_obj_t* snow_lbl,
                                           lv_obj_t* wind_lbl) {
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

  //safe_lv_label_set_text(icon_lbl, glyph.c_str());

  if (doc["temp"].is<double>() || doc["temp"].is<long>()) {
    double t = doc["temp"].as<double>();
    char tb[18];
    snprintf(tb, sizeof(tb), " %.1f°", t);
    glyph += tb;
  }
  safe_lv_label_set_text(temp_lbl, glyph.c_str());

  if (doc["pop"].is<double>() || doc["pop"].is<long>()) {
    double p = doc["pop"].as<double>();
    int pi = (int)(p * 100.0 + 0.5);
    char pb[8];
    snprintf(pb, sizeof(pb), "%d%%", pi);
    safe_lv_label_set_text(pop_lbl, pb);
  } else {
    safe_lv_label_set_text(pop_lbl, "");
  }

  // Rain: show amount (mm)) if available
  if (rain_lbl) {
    double rain_amt = 0.0;
    bool has_rain = false;
    if (doc["rain"].is<JsonObject>()) {
      if (doc["rain"]["1h"].is<double>() || doc["rain"]["1h"].is<int>()) {
        rain_amt = doc["rain"]["1h"].as<double>();
        has_rain = true;
      } else if (doc["rain"]["3h"].is<double>() || doc["rain"]["3h"].is<int>()) {
        rain_amt = doc["rain"]["3h"].as<double>();
        has_rain = true;
      }
    } else if (doc["rain"].is<double>() || doc["rain"].is<int>()) {
      rain_amt = doc["rain"].as<double>();
      has_rain = true;
    }

    if (has_rain && rain_amt > 0.0) {
      char rb[32];
      snprintf(rb, sizeof(rb), "%.1f", rain_amt);
      safe_lv_label_set_text(rain_lbl, rb);
    } else {
      safe_lv_label_set_text(rain_lbl, "");
    }
  }

  // Snow: show amount (mm) if available
  if (snow_lbl) {
    double snow_amt = 0.0;
    bool has_snow = false;
    if (doc["snow"].is<JsonObject>()) {
      if (doc["snow"]["1h"].is<double>() || doc["snow"]["1h"].is<int>()) {
        snow_amt = doc["snow"]["1h"].as<double>();
        has_snow = true;
      } else if (doc["snow"]["3h"].is<double>() || doc["snow"]["3h"].is<int>()) {
        snow_amt = doc["snow"]["3h"].as<double>();
        has_snow = true;
      }
    } else if (doc["snow"].is<double>() || doc["snow"].is<int>()) {
      snow_amt = doc["snow"].as<double>();
      has_snow = true;
    }

    if (has_snow && snow_amt > 0.0) {
      char sb[32];
      snprintf(sb, sizeof(sb), "%.1f", snow_amt);
      safe_lv_label_set_text(snow_lbl, sb);
    } else {
      safe_lv_label_set_text(snow_lbl, "");
    }
  }

  // Wind: show speed and compass direction if available
  if (wind_lbl) {
    if (doc["wind_speed"].is<double>() || doc["wind_speed"].is<int>()) {
      double ws = doc["wind_speed"].as<double>();
      std::string dir = "";
      if (doc["wind_deg"].is<double>() || doc["wind_deg"].is<int>()) {
        double deg = doc["wind_deg"].as<double>();
        dir = wind_deg_to_dir(deg);
      }
      char wb[64];
      if (!dir.empty())
        snprintf(wb, sizeof(wb), "%.1f %s", ws, dir.c_str());
      else
        snprintf(wb, sizeof(wb), "%.1f", ws);
      safe_lv_label_set_text(wind_lbl, wb);
    } else {
      safe_lv_label_set_text(wind_lbl, "");
    }
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
    if (lt) strftime(tsbuf, sizeof(tsbuf), " %H:%M ", lt);
  }
  safe_lv_label_set_text(time_label, tsbuf);
}

// Parse current conditions JSON into LV labels
static inline void pgweather_current_parse_attrs(const std::string& json,
                                                lv_obj_t* icon_lbl,
                                                lv_obj_t* temp_lbl,
                                                lv_obj_t* desc_lbl,
                                                lv_obj_t* humidity_lbl,
                                                lv_obj_t* wind_lbl) {
  if (json.empty()) return;
  JsonDocument doc;
  auto err = deserializeJson(doc, json.c_str());
  if (err) {
    ESP_LOGW("pgweather", "owm_current deserializeJson failed: %s", err.c_str());
    return;
  }

  std::string icon_code = "";
  if (doc["weather"].is<JsonArray>() && doc["weather"][0]["icon"].is<const char*>())
    icon_code = doc["weather"][0]["icon"].as<const char*>();
  else if (doc["icon"].is<const char*>())
    icon_code = doc["icon"].as<const char*>();

  std::string glyph = icon_code_to_glyph(icon_code);
  safe_lv_label_set_text(icon_lbl, glyph.c_str());

  if (temp_lbl) {
    if (doc["temp"].is<double>() || doc["temp"].is<long>()) {
      double t = doc["temp"].as<double>();
      char tb[32];
      snprintf(tb, sizeof(tb), "%.1f°", t);
      safe_lv_label_set_text(temp_lbl, tb);
    } else {
      safe_lv_label_set_text(temp_lbl, "");
    }
  }

  if (desc_lbl) {
    std::string desc = "";
    if (doc["weather"].is<JsonArray>() && doc["weather"][0]["description"].is<const char*>())
      desc = doc["weather"][0]["description"].as<const char*>();
    else if (doc["description"].is<const char* >())
      desc = doc["description"].as<const char*>();
    safe_lv_label_set_text(desc_lbl, desc.c_str());
  }

  if (humidity_lbl) {
    if (doc["humidity"].is<int>() || doc["humidity"].is<long>()) {
      int h = doc["humidity"].as<int>();
      char hb[16];
      snprintf(hb, sizeof(hb), "%d%%", h);
      safe_lv_label_set_text(humidity_lbl, hb);
    } else {
      safe_lv_label_set_text(humidity_lbl, "");
    }
  }

  if (wind_lbl) {
    if (doc["wind_speed"].is<double>() || doc["wind_speed"].is<int>()) {
      double w = doc["wind_speed"].as<double>();
      std::string dir = "";
      if (doc["wind_deg"].is<double>() || doc["wind_deg"].is<int>()) {
        double deg = doc["wind_deg"].as<double>();
        dir = wind_deg_to_dir(deg);
      }
      char wb[64];
      if (!dir.empty())
        snprintf(wb, sizeof(wb), "%.1f m/s (%s)", w, dir.c_str());
      else
        snprintf(wb, sizeof(wb), "%.1f m/s", w);
      safe_lv_label_set_text(wind_lbl, wb);
    } else {
      safe_lv_label_set_text(wind_lbl, "");
    }
  }
  
}

// Parse state (temperature) for current sensor
static inline void pgweather_current_parse_state(const std::string& state,
                                                 lv_obj_t* temp_lbl) {
  if (state.empty() || !temp_lbl) return;
  errno = 0;
  char* endptr = nullptr;
  double v = strtod(state.c_str(), &endptr);
  if (endptr == state.c_str() || errno != 0) {
    safe_lv_label_set_text(temp_lbl, "");
    return;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f°", v);
  safe_lv_label_set_text(temp_lbl, buf);
}

// Parse OpenWeatherMap "alerts" payloads into a short, display-friendly
// string and set it on the provided label. Handles JSON arrays, single
// alert objects or plain strings. Truncates long output to avoid UI overflow.
static inline void pgweather_alerts_parse_attrs(const std::string& json,
                                               lv_obj_t* alerts_lbl) {
  if (!alerts_lbl) return;
  if (json.empty()) {
    safe_lv_label_set_text(alerts_lbl, "");
    return;
  }

  // Try to parse as JSON first — sensor can provide structured alerts.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json.c_str());
  if (!err) {
    std::string out;

    // Helper: format unix timestamp to compact human readable string
    auto ts_to_string = [](long t) -> std::string {
      if (!t) return std::string("");
      time_t tt = (time_t)t;
      struct tm* lt = localtime(&tt);
      char buf[32] = {0};
      if (lt) strftime(buf, sizeof(buf), "%d.%m %H:%M", lt);
      return std::string(buf);
    };

    if (doc.is<JsonArray>()) {
      for (auto el : doc.as<JsonArray>()) {
        if (el.is<JsonObject>()) {
          const char* event = el["event"] | "";
          const char* desc = el["description"] | "";
          long start = el["start"] | 0L;
          long end = el["end"] | 0L;
          std::string piece;
          if (event && event[0]) piece += event;
          if (desc && desc[0]) {
            if (!piece.empty()) piece += ": ";
            piece += desc;
          }
          // append start/end if present
          std::string sstart = ts_to_string(start);
          std::string send = ts_to_string(end);
          if (!sstart.empty() || !send.empty()) {
            if (!piece.empty()) piece += " ";
            piece += "\n(";
            if (!sstart.empty()) piece += sstart;
            if (!sstart.empty() && !send.empty()) piece += " - ";
            if (!send.empty()) piece += send;
            piece += ")";
          }
          if (!piece.empty()) {
            if (!out.empty()) out += "\n";
            out += piece;
          }
        } else if (el.is<const char*>()) {
          if (!out.empty()) out += "; ";
          out += std::string(el.as<const char*>());
        }
      }
    } else if (doc.is<JsonObject>()) {
      const char* event = doc["event"] | "";
      const char* desc = doc["description"] | "";
      long start = doc["start"] | 0L;
      long end = doc["end"] | 0L;
      if (event && event[0]) out += event;
      if (desc && desc[0]) {
        if (!out.empty()) out += ": ";
        out += desc;
      }
      std::string sstart = ts_to_string(start);
      std::string send = ts_to_string(end);
      if (!sstart.empty() || !send.empty()) {
        if (!out.empty()) out += " ";
        out += "(";
        if (!sstart.empty()) out += sstart;
        if (!sstart.empty() && !send.empty()) out += " - ";
        if (!send.empty()) out += send;
        out += ")";
      }
    }

    if (!out.empty()) {
      // Keep label text short — truncate if necessary.
      const size_t maxlen = 512;
      if (out.size() > maxlen) out = out.substr(0, maxlen) + "…";
      safe_lv_label_set_text(alerts_lbl, out.c_str());
      return;
    }
  }

  // Fallback: treat payload as plain text (non-JSON) and display/truncate it.
  std::string s = json;
  const size_t maxlen = 200;
  if (s.size() > maxlen) s = s.substr(0, maxlen) + "…";
  safe_lv_label_set_text(alerts_lbl, s.c_str());
}

