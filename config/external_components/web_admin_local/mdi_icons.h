#ifndef MDI_ICONS_H
#define MDI_ICONS_H

#ifndef LV_FONT_FMT_TXT_LARGE
#define LV_FONT_FMT_TXT_LARGE 1
#endif
#include <lvgl.h>
#include <string>

LV_FONT_DECLARE(mdi_icons_48);
#define FONT_MDI_ICONS (&mdi_icons_48)

// Icon-Name zu Unicode-Codepoint Mapping
// Gibt den Codepoint zurück für einen Icon-Namen (z.B. "home" -> 0xF02DC)
uint32_t getMdiCodepoint(const std::string& icon_name);

// Returns true if icon name explicitly disables icon rendering (e.g. "-", "none").
bool isMdiIconDisabled(const std::string& icon_name);

// Normalizes MDI icon names (lowercase, trim, strip mdi: prefix, honor disable token).
std::string normalizeMdiIconName(const std::string& icon_name);

// Gibt ein String mit dem Unicode-Zeichen zurück (für lv_label_set_text)
std::string getMdiChar(const std::string& icon_name);

#endif // MDI_ICONS_H
