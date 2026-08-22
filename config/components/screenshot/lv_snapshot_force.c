#ifdef USE_ESP32

// Force snapshot symbols to be emitted for this translation unit.
#undef LV_USE_SNAPSHOT
#define LV_USE_SNAPSHOT 1

// ESPHome 2026.7.0 on this target links LVGL without snapshot symbols.
// Include LVGL's snapshot implementation in this component so custom
// screenshot capture can use lv_snapshot_* APIs at runtime.
#include "src/draw/snapshot/lv_snapshot.c"

#endif
