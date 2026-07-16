/**
 * Take a snapshot of an LVGL object and write the image into a user-provided buffer.
 *
 * Captures the visual content of the given object (including its extended draw size)
 * into the provided buffer using the specified image color format. The function
 * constructs a temporary "fake" display/driver and draw context to render the object
 * into the buffer without disturbing the real display output.
 *
 * Preconditions / Requirements:
 * - The object pointer (obj), destination image descriptor pointer (dsc) and buffer (buf)
 *   must be non-NULL.
 * - The buffer must be large enough: buf_size >= lv_snapshot_buf_size_needed(obj, cf).
 * - Only the following color formats are supported:
 *   LV_IMG_CF_TRUE_COLOR, LV_IMG_CF_TRUE_COLOR_ALPHA,
 *   LV_IMG_CF_ALPHA_1BIT, LV_IMG_CF_ALPHA_2BIT, LV_IMG_CF_ALPHA_4BIT, LV_IMG_CF_ALPHA_8BIT.
 *
 * Behavior / Side effects:
 * - Determines the snapshot area from the object's coordinates and its ext_draw_size;
 *   the resulting image width/height include the ext_size on all sides.
 * - Temporarily sets the "refreshing" display pointer to an internally created fake display
 *   so the object's drawing routines render into the provided buffer.
 * - Allocates a draw context (using lv_mem_alloc) and frees it prior to returning.
 * - Calls lv_obj_redraw for the target object, the top layer and the system layer while
 *   the fake display is active.
 * - Fills the provided lv_img_dsc_t structure (dsc->data, dsc->data_size, dsc->header.w,
 *   dsc->header.h, dsc->header.cf) on success.
 *
 * Parameters:
 * - obj: Pointer to the LVGL object to snapshot.
 * - cf: Desired image color format (one of the supported LV_IMG_CF_* values listed above).
 * - dsc: Pointer to an lv_img_dsc_t structure that will be populated with the image
 *        descriptor on success. The caller must provide a valid struct.
 * - buf: Pointer to a writable buffer where the image bytes will be written.
 * - buf_size: Size in bytes of the provided buffer.
 *
 * Returns:
 * - LV_RES_OK on success (dsc populated, image data written into buf).
 * - LV_RES_INV if any argument is invalid, the color format is unsupported, the buffer
 *   is too small, or an internal allocation/initialization fails.
 *
 * Notes:
 * - The function uses assertions (LV_ASSERT_NULL) to validate pointers in debug builds.
 * - The caller remains responsible for the lifetime of the provided buffer; dsc->data
 *   will point into this buffer.
 * - This routine is not intended to be reentrant with respect to display refreshes;
 *   avoid concurrent modifications to the same objects or display while calling.
 * - this function is based on lv_snapshot_take_to_buf but extended to capture top and sys layers, too
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif
#if defined(LV_USE_SNAPSHOT) && LV_USE_SNAPSHOT
lv_result_t lv_snapshot_take_to_buf(lv_obj_t *obj, lv_color_format_t cf, lv_image_dsc_t *dsc, void *buf,
                                    uint32_t buf_size);
#endif
#ifdef __cplusplus
}
#endif

static lv_result_t snapshot_take_to_buf_compat(lv_obj_t *obj, lv_color_format_t cf, lv_image_dsc_t *dsc, void *buf,
                                               uint32_t buf_size) {
#if defined(LV_USE_SNAPSHOT) && LV_USE_SNAPSHOT
  return lv_snapshot_take_to_buf(obj, cf, dsc, buf, buf_size);
#else
  LV_UNUSED(obj);
  LV_UNUSED(cf);
  LV_UNUSED(dsc);
  LV_UNUSED(buf);
  LV_UNUSED(buf_size);
  return LV_RESULT_INVALID;
#endif
}

static bool get_snapshot_area_from_dsc(lv_obj_t *obj, const lv_image_dsc_t *dsc, lv_area_t *snapshot_area) {
  if (obj == nullptr || dsc == nullptr || snapshot_area == nullptr) return false;

  lv_obj_update_layout(obj);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);

  int32_t obj_w = coords.x2 - coords.x1 + 1;
  int32_t obj_h = coords.y2 - coords.y1 + 1;
  int32_t ext_x = ((int32_t) dsc->header.w - obj_w) / 2;
  int32_t ext_y = ((int32_t) dsc->header.h - obj_h) / 2;

  snapshot_area->x1 = coords.x1 - ext_x;
  snapshot_area->y1 = coords.y1 - ext_y;
  snapshot_area->x2 = snapshot_area->x1 + (int32_t) dsc->header.w - 1;
  snapshot_area->y2 = snapshot_area->y1 + (int32_t) dsc->header.h - 1;
  return true;
}

static void blend_layer_argb8888_on_rgb565(lv_obj_t *layer_obj, void *base_buf, lv_image_dsc_t *base_dsc,
                                           const lv_area_t *base_area) {
  if (layer_obj == nullptr || base_buf == nullptr || base_dsc == nullptr || base_area == nullptr) return;
  if (base_dsc->header.cf != LV_COLOR_FORMAT_RGB565) return;

  lv_obj_update_layout(layer_obj);
  lv_area_t layer_coords;
  lv_obj_get_coords(layer_obj, &layer_coords);
  int32_t layer_w = layer_coords.x2 - layer_coords.x1 + 1;
  int32_t layer_h = layer_coords.y2 - layer_coords.y1 + 1;
  if (layer_w <= 0 || layer_h <= 0) return;

  uint32_t overlay_buf_size = (uint32_t) layer_w * (uint32_t) layer_h * sizeof(lv_color32_t);
  void *overlay_buf = lv_malloc(overlay_buf_size);
  if (overlay_buf == nullptr) return;

  lv_image_dsc_t overlay_dsc;
  lv_memzero(&overlay_dsc, sizeof(overlay_dsc));
  lv_result_t overlay_res = snapshot_take_to_buf_compat(layer_obj, LV_COLOR_FORMAT_ARGB8888, &overlay_dsc, overlay_buf,
                                                        overlay_buf_size);
  if (overlay_res != LV_RESULT_OK || overlay_dsc.data == nullptr) {
    lv_free(overlay_buf);
    return;
  }

  lv_area_t overlay_area;
  if (!get_snapshot_area_from_dsc(layer_obj, &overlay_dsc, &overlay_area)) {
    lv_free(overlay_buf);
    return;
  }

  lv_area_t blend_area;
  blend_area.x1 = LV_MAX(base_area->x1, overlay_area.x1);
  blend_area.y1 = LV_MAX(base_area->y1, overlay_area.y1);
  blend_area.x2 = LV_MIN(base_area->x2, overlay_area.x2);
  blend_area.y2 = LV_MIN(base_area->y2, overlay_area.y2);
  if (blend_area.x1 > blend_area.x2 || blend_area.y1 > blend_area.y2) {
    lv_free(overlay_buf);
    return;
  }

  uint16_t *base_px = reinterpret_cast<uint16_t *>(base_buf);
  const lv_color32_t *overlay_px = reinterpret_cast<const lv_color32_t *>(overlay_dsc.data);
  const int32_t base_w = static_cast<int32_t>(base_dsc->header.w);
  const int32_t overlay_w = static_cast<int32_t>(overlay_dsc.header.w);

  for (int32_t y = blend_area.y1; y <= blend_area.y2; y++) {
    const int32_t base_row = y - base_area->y1;
    const int32_t overlay_row = y - overlay_area.y1;

    for (int32_t x = blend_area.x1; x <= blend_area.x2; x++) {
      const int32_t base_col = x - base_area->x1;
      const int32_t overlay_col = x - overlay_area.x1;

      lv_color32_t fg = overlay_px[overlay_row * overlay_w + overlay_col];
      if (fg.alpha == LV_OPA_TRANSP) continue;

      uint16_t *bg = &base_px[base_row * base_w + base_col];
      uint16_t fg_565 = lv_color_to_u16(lv_color_make(fg.red, fg.green, fg.blue));
      if (fg.alpha >= LV_OPA_COVER) {
        *bg = fg_565;
      } else {
        *bg = lv_color_16_16_mix(fg_565, *bg, fg.alpha);
      }
    }
  }

  lv_free(overlay_buf);
}


lv_result_t lv_snapshot_take_to_buf_ex(lv_obj_t *obj, lv_color_format_t cf, lv_image_dsc_t *dsc, void *buf,
                                       uint32_t buf_size) {
  LV_ASSERT_NULL(obj);
  LV_ASSERT_NULL(dsc);
  LV_ASSERT_NULL(buf);

  lv_memzero(dsc, sizeof(lv_image_dsc_t));
  lv_result_t res = snapshot_take_to_buf_compat(obj, cf, dsc, buf, buf_size);
  if (res != LV_RESULT_OK) return res;

  if (cf != LV_COLOR_FORMAT_RGB565) return res;

  lv_area_t base_area;
  if (!get_snapshot_area_from_dsc(obj, dsc, &base_area)) return res;

  lv_display_t *disp = lv_obj_get_display(obj);
  if (disp == nullptr) return res;

  lv_obj_t *top = lv_display_get_layer_top(disp);
  lv_obj_t *sys = lv_display_get_layer_sys(disp);

  if (top != nullptr && top != obj) {
    blend_layer_argb8888_on_rgb565(top, buf, dsc, &base_area);
  }

  if (sys != nullptr && sys != obj && sys != top) {
    blend_layer_argb8888_on_rgb565(sys, buf, dsc, &base_area);
  }

  return res;
}
