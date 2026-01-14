#pragma once
#include <string>
#include "esphome.h"
#include "esphome/components/lvgl/lvgl_esphome.h"

// Inline, static implementation to ensure the parser is available
// to the single translation unit that includes this header.
static inline void safe_lv_label_set_text(lv_obj_t *label, const char *txt) {
    if (!label) return;
    if (!txt) txt = "";
    lv_label_set_text(label, txt);
}

static const char *mdi_clear_night = "\U0000EA62";
static const char *mdi_cloudy = "\U0000E2BD";
static const char *mdi_fog = "\U0000E818";
static const char *mdi_lightning_rainy = "\U0000F61E";
static const char *mdi_pouring = "\U0000F61F";
static const char *mdi_rainy = "\U0000F176";
static const char *mdi_snowy = "\U0000E80F";
static const char *mdi_snowy_night = "\U0000E2CD";
static const char *mdi_snowy_rainy = "\U0000F61D";
static const char *mdi_sunny = "\U0000E81A";
static const char *mdi_unknown = "\U0000E6A5";

static inline void pgweather_hourly_parser(const std::string &json,
                                                                                     lv_obj_t *time_label,
                                                                                     lv_obj_t *icon_lbl,
                                                                                     lv_obj_t *temp_lbl,
                                                                                     lv_obj_t *pop_lbl) {
    if (json.empty())
        return;

    StaticJsonDocument<512> doc;
    auto err = deserializeJson(doc, json.c_str());
    if (err) {
        ESP_LOGW("pgweather", "owm_hourly_parse failed: %s", err.c_str());
        return;
    }

    if (doc["dt_txt"].is<const char*>()) {
        const char *s = doc["dt_txt"].as<const char*>();
        safe_lv_label_set_text(time_label, s);
    } else if (doc["dt"].is<long>()) {
        long dt = doc["dt"].as<long>();
        char tsbuf[16] = {0};
        if (dt) {
            time_t t = (time_t)dt;
            struct tm *lt = localtime(&t);
            if (lt) strftime(tsbuf, sizeof(tsbuf), "%H:%M", lt);
        }
        safe_lv_label_set_text(time_label, tsbuf);
    }

    std::string icon_code = "";
    if (doc["weather"].is<JsonArray>() && doc["weather"][0]["icon"].is<const char*>())
        icon_code = doc["weather"][0]["icon"].as<const char*>();
    else if (doc["icon"].is<const char*>())
        icon_code = doc["icon"].as<const char*>();

    std::string glyph = std::string(mdi_unknown);
    if (icon_code == "01d") glyph = std::string(mdi_sunny);
    else if (icon_code == "01n") glyph = std::string(mdi_clear_night);
    else if (icon_code == "02d" || icon_code == "02n") glyph = std::string(mdi_cloudy);
    else if (icon_code == "03d" || icon_code == "03n" || icon_code == "04d" || icon_code == "04n") glyph = std::string(mdi_cloudy);
    else if (icon_code == "09d" || icon_code == "09n") glyph = std::string(mdi_pouring);
    else if (icon_code == "10d" || icon_code == "10n") glyph = std::string(mdi_rainy);
    else if (icon_code == "11d" || icon_code == "11n") glyph = std::string(mdi_lightning_rainy);
    else if (icon_code == "13d" || icon_code == "13n") glyph = std::string(mdi_snowy);
    else if (icon_code == "50d" || icon_code == "50n") glyph = std::string(mdi_fog);

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
