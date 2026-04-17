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


lv_result_t lv_snapshot_take_to_buf_ex(lv_obj_t *obj, lv_color_format_t cf, lv_image_dsc_t *dsc, void *buf,
                                       uint32_t buf_size) {
  LV_ASSERT_NULL(obj);
  LV_ASSERT_NULL(dsc);
  LV_ASSERT_NULL(buf);

  lv_memzero(dsc, sizeof(lv_image_dsc_t));
  return lv_snapshot_take_to_buf(obj, cf, dsc, buf, buf_size);
}
