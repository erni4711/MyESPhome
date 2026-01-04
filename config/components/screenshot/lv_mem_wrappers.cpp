// Lightweight LVGL memory wrapper implementations for LV_MEM_CUSTOM hooks
#include <stdlib.h>
#include <esp_heap_caps.h>

extern "C" {

void *my_lvgl_malloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
}

void my_lvgl_free(void *ptr) {
  heap_caps_free(ptr);
}

void *my_lvgl_realloc(void *ptr, size_t size) {
  return heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
}

} // extern "C"
