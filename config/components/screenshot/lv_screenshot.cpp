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


lv_res_t lv_snapshot_take_to_buf_ex (lv_obj_t * obj, lv_img_cf_t cf, lv_img_dsc_t * dsc, void * buf, uint32_t buf_size)
{
    LV_ASSERT_NULL(obj);
    LV_ASSERT_NULL(dsc);
    LV_ASSERT_NULL(buf);

    switch(cf) {
        case LV_IMG_CF_TRUE_COLOR:
        case LV_IMG_CF_TRUE_COLOR_ALPHA:
        case LV_IMG_CF_ALPHA_1BIT:
        case LV_IMG_CF_ALPHA_2BIT:
        case LV_IMG_CF_ALPHA_4BIT:
        case LV_IMG_CF_ALPHA_8BIT:
            break;
        default:
            return LV_RES_INV;
    }

    uint32_t buf_size_needed = lv_snapshot_buf_size_needed(obj, cf);
    if(buf_size_needed == 0 || buf_size < buf_size_needed) return LV_RES_INV;

    /*Width and height determine snapshot image size.*/
    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj);
    lv_coord_t ext_size = _lv_obj_get_ext_draw_size(obj);
    w += ext_size * 2;
    h += ext_size * 2;

    lv_area_t snapshot_area;
    lv_obj_get_coords(obj, &snapshot_area);
    lv_area_increase(&snapshot_area, ext_size, ext_size);

    lv_memset(buf, 0x00, buf_size);
    lv_memset_00(dsc, sizeof(lv_img_dsc_t));

    lv_disp_t * obj_disp = lv_obj_get_disp(obj);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    /*In lack of a better idea use the resolution of the object's display*/
    driver.hor_res = lv_disp_get_hor_res(obj_disp);
    driver.ver_res = lv_disp_get_hor_res(obj_disp);
    lv_disp_drv_use_generic_set_px_cb(&driver, cf);

    lv_disp_t fake_disp;
    lv_memset_00(&fake_disp, sizeof(lv_disp_t));
    fake_disp.driver = &driver;

    lv_draw_ctx_t * draw_ctx = (lv_draw_ctx_t *)lv_mem_alloc(obj_disp->driver->draw_ctx_size);
    LV_ASSERT_MALLOC(draw_ctx);
    if(draw_ctx == NULL) return LV_RES_INV;
    obj_disp->driver->draw_ctx_init(fake_disp.driver, draw_ctx);
    fake_disp.driver->draw_ctx = draw_ctx;
    draw_ctx->clip_area = &snapshot_area;
    draw_ctx->buf_area = &snapshot_area;
    draw_ctx->buf = (uint8_t *)buf;
    driver.draw_ctx = draw_ctx;

    lv_disp_t * refr_ori = _lv_refr_get_disp_refreshing();
    _lv_refr_set_disp_refreshing(&fake_disp);

    lv_obj_redraw(draw_ctx, obj);
    lv_obj_redraw(draw_ctx, lv_layer_top());
    lv_obj_redraw(draw_ctx, lv_layer_sys());  

    _lv_refr_set_disp_refreshing(refr_ori);
    obj_disp->driver->draw_ctx_deinit(fake_disp.driver, draw_ctx);
    lv_mem_free(draw_ctx);

    dsc->data = (uint8_t *)buf;
    dsc->data_size = buf_size_needed;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = cf;
    return LV_RES_OK;
}
