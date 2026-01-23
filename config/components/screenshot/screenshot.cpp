
#ifdef USE_ESP32

#include "screenshot.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#if defined(__cplusplus)
extern "C" {
void *my_lvgl_malloc(size_t size);
void my_lvgl_free(void *ptr);
void *my_lvgl_realloc(void *ptr, size_t size);
}
#endif
#include <string>
#include <stdio.h>
#include <esp_http_server.h>
#include <vector>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
// JPEG encoder removed; keep PPM path only

// libpng streaming removed; use zlib-based writer or stb fallback instead

// Try to use zlib (deflate) for on-the-fly IDAT generation
#if defined(__has_include)
#  if __has_include(<zlib.h>)
#    include <zlib.h>
#    define HAVE_ZLIB 1
#  endif
#endif

// try to use heap_caps_malloc() when available (PSRAM)
#if defined(__has_include)
#  if __has_include(<esp_heap_caps.h>)
#    include <esp_heap_caps.h>
#    define HAVE_HEAP_CAPS 1
#  endif
#endif

#include "PNGenc.h"
// try to declare encoder API if available
// Always use the bundled stb_image_write for JPEG encoding.

// registration wrapper (C++ linkage)
void screenshot_register_runtime() {
  esphome::screenshot::ScreenshotComponent::register_component_runtime();
}

lv_res_t lv_snapshot_take_to_buf_ex (lv_obj_t * obj, lv_img_cf_t cf, lv_img_dsc_t * dsc, void * buf, uint32_t buf_size);
namespace esphome {
namespace screenshot {

static const char *TAG = "screenshot";

// When using LVGL snapshot API we can avoid copying by using the image's data
// pointer and freeing the lv_img_dsc_t when done. Store the last snapshot here.
// (no fallback contiguous buffer)


// Helper: grab LVGL active screen buffer pointer and size. This is best-effort.
static lv_img_dsc_t *grab_lvgl_rgb565() {
  lv_obj_t *scr = lv_scr_act();
  ESP_LOGD(TAG, "grab_lvgl_rgb565: lv_scr_act -> %p", (void *)scr);
  /* Determine required buffer size for RGB565 snapshot */
  uint32_t buf_size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
  if (buf_size == 0) {
    ESP_LOGD(TAG, "grab_lvgl_rgb565: snapshot buffer size is 0");
    return nullptr;
  }

  /* Allocate buffer directly with the project's LVGL hooks (my_lvgl_malloc) */
  void *buf = my_lvgl_malloc(buf_size);
  if (!buf) {
    ESP_LOGW(TAG, "grab_lvgl_rgb565: failed to allocate %u bytes for snapshot", (unsigned)buf_size);
    return nullptr;
  }

  /* Allocate an image descriptor via LVGL allocator */
  lv_img_dsc_t *dsc = (lv_img_dsc_t *)my_lvgl_malloc(sizeof(lv_img_dsc_t));
  if (!dsc) {
    ESP_LOGW(TAG, "grab_lvgl_rgb565: failed to allocate lv_img_dsc_t");
    my_lvgl_free(buf);
    return nullptr;
  }

  /* Take snapshot into our buffer */
  lv_refr_now(NULL);
  lv_res_t res = lv_snapshot_take_to_buf_ex(scr, LV_IMG_CF_TRUE_COLOR, dsc, buf, buf_size);
  if (res != LV_RES_OK) {
    ESP_LOGW(TAG, "grab_lvgl_rgb565: lv_snapshot_take_to_buf failed");
    my_lvgl_free(buf);
    my_lvgl_free(dsc);
    return nullptr;
  }


  /* On success lv_snapshot_take_to_buf wrote the dsc and buffer for us */
  return dsc;
}

// Response that owns a buffer and frees it when destroyed.
class OwnedProgmemResponse : public ::esphome::web_server_idf::AsyncWebServerResponse {
 public:
  OwnedProgmemResponse(const ::esphome::web_server_idf::AsyncWebServerRequest *req, uint8_t *data, size_t size)
      : AsyncWebServerResponse(req), data_(data), size_(size) {}
  ~OwnedProgmemResponse() override { if (this->data_) my_lvgl_free(this->data_); }
  const char *get_content_data() const override { return reinterpret_cast<const char *>(this->data_); }
  size_t get_content_size() const override { return this->size_; }

 private:
  uint8_t *data_;
  size_t size_;
};

// Response that owns an lv_img_dsc_t snapshot and frees it when destroyed.
class OwnedImageResponse : public ::esphome::web_server_idf::AsyncWebServerResponse {
 public:
  OwnedImageResponse(const ::esphome::web_server_idf::AsyncWebServerRequest *req, lv_img_dsc_t *img)
      : AsyncWebServerResponse(req), img_(img) {}
  ~OwnedImageResponse() override {
    if (this->img_) {
      lv_snapshot_free(this->img_);
      this->img_ = nullptr;
    }
  }
  const char *get_content_data() const override { return reinterpret_cast<const char *>(this->img_->data); }
  size_t get_content_size() const override { return (size_t)this->img_->header.w * (size_t)this->img_->header.h * 3; }

 private:
  lv_img_dsc_t *img_;
};

// Note: contiguous PPM builder removed. The handler now streams a PPM header
// followed by the lv_img_dsc_t pixel data when the snapshot is RGB888. If the
// snapshot format is not RGB888 the handler will return an error.

// JPEG encoding removed; PPM-only implementation retained.

void ScreenshotComponent::setup() {
  ESP_LOGI(TAG, "Setting up ScreenshotComponent");

  if (web_server_base::global_web_server_base == nullptr) {
    ESP_LOGE(TAG, "No web_server_base available; cannot register /screenshot.jpg handler");
    return;
  }

  this->handler_ = new Handler(this);
  web_server_base::global_web_server_base->add_handler(this->handler_);
  // Create binary semaphore for synchronous capture signalling
  if (this->capture_done_ == nullptr) {
    this->capture_done_ = xSemaphoreCreateBinary();
  }
  if (this->capture_done_) {
    ESP_LOGD(TAG, "capture_done_ semaphore created %p", (void *)this->capture_done_);
  }
  ESP_LOGI(TAG, "Registered /screenshot.png handler with web_server");
}

void ScreenshotComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Screenshot endpoint: /screenshot.jpg (via web_server)");
}

// Forward declaration for synchronous handler helper
static void process_request(AsyncWebServerRequest *request);

void ScreenshotComponent::Handler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI(TAG, "HTTP /screenshot.png request via web_server");
  ESP_LOGD(TAG, "Request URL='%s' Method=%d", request->url().c_str(), request->method());
  // Post capture request to main loop and wait for result.
  ESP_LOGD(TAG, "Posting capture request to main loop and waiting for result");

  // Ensure semaphore exists
  if (this->parent_->capture_done_ == nullptr) {
    this->parent_->capture_done_ = xSemaphoreCreateBinary();
    if (this->parent_->capture_done_ == nullptr) {
      ESP_LOGW(TAG, "Failed to create capture semaphore");
      request->send(500, "text/plain", "Internal error");
      return;
    }
  }

  // Arm request and signal main loop to perform capture
  this->parent_->capture_result_ = nullptr;
  this->parent_->processing_ = true;
  this->parent_->pending_request_ = request;
  ESP_LOGD(TAG, "Posted capture request to main loop; waiting up to %u ms", (unsigned)2000);

  // Wait for main loop to complete capture (timeout 2s)
  const TickType_t timeout = pdMS_TO_TICKS(2000);
  if (xSemaphoreTake(this->parent_->capture_done_, timeout) != pdTRUE) {
    ESP_LOGW(TAG, "Capture timeout waiting for main loop");
    this->parent_->processing_ = false;
    this->parent_->pending_request_ = nullptr;
    request->send(504, "text/plain", "Capture timeout");
    return;
  }

  // Retrieve captured image produced on main thread
  lv_img_dsc_t *img = this->parent_->capture_result_;
  // Clear pending state
  this->parent_->capture_result_ = nullptr;
  this->parent_->processing_ = false;
  this->parent_->pending_request_ = nullptr;

  if (!img) {
    ESP_LOGW(TAG, "Main-loop capture failed");
    request->send(500, "text/plain", "Could not grab LVGL framebuffer\n");
    return;
  }
  // From here, stream the captured image (reuse streaming code path)
  ESP_LOGD(TAG, "Processing captured image on HTTP task");
  ESP_LOGI(TAG, "Captured image: w=%u h=%u cf=%d data=%p", (unsigned)img->header.w, (unsigned)img->header.h,
           img->header.cf, img->data);

  uint32_t w = img->header.w;
  uint32_t h = img->header.h;
  uint32_t pixels = w * h;
  std::string header = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";

  httpd_req_t *req = *request;
  httpd_resp_set_status(req, HTTPD_200);
  char wbuf[16];
  char hbuf[16];
  snprintf(wbuf, sizeof(wbuf), "%u", (unsigned)w);
  snprintf(hbuf, sizeof(hbuf), "%u", (unsigned)h);
  httpd_resp_set_hdr(req, "X-Image-Width", wbuf);
  httpd_resp_set_hdr(req, "X-Image-Height", hbuf);

  /* chunked send helper */
  auto send_chunk_with_retry = [&](httpd_req_t *req, const char *data, ssize_t len) -> esp_err_t {
    const int max_retries = 3;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
      err = httpd_resp_send_chunk(req, data, len);
      if (err == ESP_OK) return err;
      ESP_LOGW(TAG, "httpd_resp_send_chunk attempt %d failed: 0x%04x (%s)", attempt + 1, (int)err,
               esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    return err;
  };

  size_t header_len = header.size();

  const uint8_t *src = reinterpret_cast<const uint8_t *>(img->data);

  // Try to initialize PNG encoder first. If that fails we'll fall back to
  // streaming a PPM and send the PPM header only in that case (avoids
  // corrupting PNG output with a preceding PPM header).
  size_t row_bytes = (size_t)w * 3;
  uint8_t *rowbuf = (uint8_t *)my_lvgl_malloc(row_bytes);
  if (!rowbuf) {
    ESP_LOGW(TAG, "Failed to allocate per-row buffer of %u bytes", (unsigned)row_bytes);
    my_lvgl_free((void *)img->data);
    my_lvgl_free(img);
    return;
  }
  ESP_LOGD(TAG, "Allocated per-row buffer of %u bytes", (unsigned)row_bytes);
  
  size_t png_bytes = (size_t)w * (size_t)h * 3;
  uint8_t *png_buf = (uint8_t *) my_lvgl_malloc(png_bytes);
  if (!png_buf) {
    ESP_LOGW(TAG, "Failed to allocate pngbuf of %u bytes", (unsigned)png_bytes);
    my_lvgl_free((void *)img->data);
    my_lvgl_free(img);
    return;
  }
  ESP_LOGD(TAG, "Allocated png buffer of %u bytes", (unsigned)png_bytes);
  PNGenc png_encoder;
  int rc = png_encoder.open(png_buf, png_bytes);
  if (rc != PNG_SUCCESS) {
    ESP_LOGW(TAG, "Failed to open PNG encoder: %d", rc);
  }
  if (rc == PNG_SUCCESS)
  {
    rc = png_encoder.encodeBegin(w, h, PNG_PIXEL_TRUECOLOR, 24, NULL, 9);
    if (rc != PNG_SUCCESS) {
      ESP_LOGW(TAG, "Error starting PNG encoding = %d\n", rc);
    } 
  }

  // Select content-type and send header if falling back to PPM
  if (rc == PNG_SUCCESS) {
    httpd_resp_set_type(req, "image/png");
  } else {
    httpd_resp_set_type(req, "image/x-portable-pixmap");
    if (send_chunk_with_retry(req, header.c_str(), (ssize_t)header_len) != ESP_OK) {
      ESP_LOGW(TAG, "Failed to send PPM header");
      my_lvgl_free((void *)png_buf);
      my_lvgl_free((void *)img->data);
      my_lvgl_free(img);
      my_lvgl_free(rowbuf);
      return;
    }
  }

  auto black = lv_color_make(0, 0, 0);
  ESP_LOGD(TAG, "Starting to convert and send image %d rows", (unsigned)h);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      lv_color_t color = lv_img_buf_get_px_color(img, x, y, black);
      size_t idx = (size_t)x * 3;
      auto r = LV_COLOR_GET_R(color);
      auto g = LV_COLOR_GET_G(color);
      auto b = LV_COLOR_GET_B(color); 
      rowbuf[idx + 0] = (r << 3) | (r ? 7 : 0);
      rowbuf[idx + 1] = (g << 2) | (g ? 3 : 0);
      rowbuf[idx + 2] = (b << 3) | (b ? 7 : 0);
    }
    if (rc == PNG_SUCCESS) {
      rc = png_encoder.addLine(rowbuf);
      if (rc != PNG_SUCCESS) {
        ESP_LOGW(TAG, "Error adding line %u to PNG encoding = %d\n", (unsigned)y,rc);
      } 
    } else {
      if (send_chunk_with_retry(req, reinterpret_cast<const char *>(rowbuf), (ssize_t)row_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send converted row %u", (unsigned)y);
        break;
      }
    }
  }
  if (rc == PNG_SUCCESS) {
    ESP_LOGD(TAG, "Finalizing PNG image");
    size_t bytes_written = png_encoder.close();
    if (this->parent_->sd_spi_card_ != nullptr) {
      const std::string path = "/sdcard/screenshot.png";
      bool mounted = this->parent_->sd_spi_card_->is_mounted();
      if (!mounted) {
        mounted = this->parent_->sd_spi_card_->mount();
      }
      if (!mounted) {
        ESP_LOGW(TAG, "SD mount failed; skipping write");
      } else {
        this->parent_->sd_spi_card_->delete_file(path);
        bool ok = this->parent_->sd_spi_card_->append_file_chunk(path, png_buf, bytes_written, true);
        if (!ok) {
          ESP_LOGW(TAG, "SD write failed: %s", path.c_str());
        } else {
          ESP_LOGD(TAG, "Wrote PNG to %s", path.c_str());
        }
      }
    } else {
      ESP_LOGD(TAG, "sd_spi_card not configured, skipping SD write");
    }
    send_chunk_with_retry(req, reinterpret_cast<const char *>(png_buf), (ssize_t)bytes_written);
    ESP_LOGD(TAG, "PNG image finalized, %u bytes written", (unsigned)bytes_written);
  }
  httpd_resp_send_chunk(req, NULL, 0);
  ESP_LOGD(TAG, "Completed sending PPM image data");
  my_lvgl_free((void *)png_buf);
  my_lvgl_free((void *)img->data);
  my_lvgl_free(img);
  my_lvgl_free(rowbuf);
  return;
}

 

// No queued processing — request handling is synchronous in the handler now.
void ScreenshotComponent::loop() {
  // If a capture was requested by the HTTP handler, perform it here on the main thread
  if (!this->processing_)
    return;

  // Perform LVGL capture on the main thread
  lv_img_dsc_t *img = grab_lvgl_rgb565();

  // Store result and signal HTTP task
  this->capture_result_ = img;
  this->processing_ = false;
  if (this->capture_done_) {
    xSemaphoreGive(this->capture_done_);
  }
  if (img) {
    ESP_LOGD(TAG, "Main-loop capture stored img=%p w=%u h=%u cf=%d", (void *)img, (unsigned)img->header.w,
             (unsigned)img->header.h, img->header.cf);
  } else {
    ESP_LOGW(TAG, "Main-loop capture returned NULL image");
  }
}

}  // namespace screenshot
}  // namespace esphome

#endif  // USE_ESP32
