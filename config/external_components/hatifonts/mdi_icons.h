#ifndef MDI_ICONS_H
#define MDI_ICONS_H

#ifndef LV_FONT_FMT_TXT_LARGE
#define LV_FONT_FMT_TXT_LARGE 1
#endif
#include <lvgl.h>
#include <string>

#ifndef MDI_ICONS_48
#define MDI_ICONS_48 1
#endif
#ifndef MDI_ICONS_40
#define MDI_ICONS_40 1
#endif
#ifndef MDI_ICONS_32
#define MDI_ICONS_32 1
#endif
#ifndef MDI_ICONS_40_REDUCED
#define MDI_ICONS_40_REDUCED 0  
#endif

#if MDI_ICONS_48
LV_FONT_DECLARE(mdi_icons_48);
#endif
#if MDI_ICONS_40
LV_FONT_DECLARE(mdi_icons_40);
#endif
#if MDI_ICONS_32
LV_FONT_DECLARE(mdi_icons_32);
#endif

#if MDI_ICONS_40_REDUCED
LV_FONT_DECLARE(mdi_icons_40_reduced);
#endif

inline const lv_font_t *mdi_font_for_display() {
  const lv_display_t *display = lv_display_get_default();
  if (display == nullptr) {
#if MDI_ICONS_48
    return &mdi_icons_48;
#else
#if MDI_ICONS_40_REDUCED
  return &mdi_icons_40_reduced;
#else
  return &mdi_icons_40;
#endif
#endif
  }
  const int width = lv_display_get_horizontal_resolution(display);
  const int height = lv_display_get_vertical_resolution(display);
  const int largest_dimension = width > height ? width : height;
#if MDI_ICONS_32
  if (largest_dimension <= 480) return &mdi_icons_32;
#endif
#if MDI_ICONS_40_REDUCED
  if (largest_dimension <= 800) return &mdi_icons_40_reduced;
#else
  if (largest_dimension <= 800) return &mdi_icons_40;
#endif
#if MDI_ICONS_48
  return &mdi_icons_48;
#else
#if MDI_ICONS_40_REDUCED
  return &mdi_icons_40_reduced;
#else
  return &mdi_icons_40;
#endif
#endif
}

#define FONT_MDI_ICONS (mdi_font_for_display())

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
