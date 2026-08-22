
//#ifndef LV_CONF_H
//#define LV_CONF_H
#include <stddef.h>
/* Keep other defaults — this file intentionally minimal. If you need more LVGL
 * configuration overrides add them here.
 */
/* Minimal LVGL config enabling snapshot support for snapshot-based framebuffer capture */
#define LV_USE_SNAPSHOT 1

#define LV_FONT_MONTSERRAT_16 1

#define LV_MEM_CUSTOM 1
void *my_lvgl_malloc(size_t size);
void my_lvgl_free(void *ptr); 
void *my_lvgl_realloc(void *ptr, size_t size);
#define LV_MEM_CUSTOM_ALLOC   my_lvgl_malloc
#define LV_MEM_CUSTOM_FREE    my_lvgl_free
#define LV_MEM_CUSTOM_REALLOC my_lvgl_realloc

#define LV_USE_LOG 1
//#define LV_LOG_LEVEL LV_LOG_LEVEL_TRACE

//#endif // LV_CONF_H
