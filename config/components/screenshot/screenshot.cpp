
#ifdef USE_ESP32

#include "screenshot.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#if defined(__has_include)
#  if __has_include("esphome/components/sd_mmc_card/sd_mmc_card.h")
#    include "esphome/components/sd_mmc_card/sd_mmc_card.h"
#    define HAVE_SD_MMC_CARD 1
#  endif
#endif
#ifndef HAVE_SD_MMC_CARD
#  define HAVE_SD_MMC_CARD 0
#endif

#if defined(__cplusplus)
extern "C" {
void *my_lvgl_malloc(size_t size);
void my_lvgl_free(void *ptr);
void *my_lvgl_realloc(void *ptr, size_t size);
}
#endif
#include <string>
#include <stdio.h>
#include <cstring>
#include <esp_http_server.h>
#include <esp_task_wdt.h>
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

static bool encode_png_to_buffer(lv_img_dsc_t *img, uint8_t **out_buf, size_t *out_size) {
  if (!img || !out_buf || !out_size) return false;
  uint32_t w = img->header.w;
  uint32_t h = img->header.h;
  size_t row_bytes = (size_t) w * 3;
  uint8_t *rowbuf = (uint8_t *) my_lvgl_malloc(row_bytes);
  if (!rowbuf) {
    ESP_LOGW(TAG, "Failed to allocate per-row buffer of %u bytes", (unsigned) row_bytes);
    return false;
  }

  size_t png_bytes = (size_t) w * (size_t) h; 
  uint8_t *png_buf = (uint8_t *) my_lvgl_malloc(png_bytes);
  if (!png_buf) {
    ESP_LOGW(TAG, "Failed to allocate pngbuf of %u bytes", (unsigned) png_bytes);
    my_lvgl_free(rowbuf);
    return false;
  }

  PNGenc png_encoder;
  int rc = png_encoder.open(png_buf, png_bytes);
  if (rc != PNG_SUCCESS) {
    ESP_LOGW(TAG, "Failed to open PNG encoder: %d", rc);
    my_lvgl_free(rowbuf);
    my_lvgl_free(png_buf);
    return false;
  }
  rc = png_encoder.encodeBegin(w, h, PNG_PIXEL_TRUECOLOR, 24, NULL, 1);
  if (rc != PNG_SUCCESS) {
    ESP_LOGW(TAG, "Error starting PNG encoding = %d", rc);
    my_lvgl_free(rowbuf);
    my_lvgl_free(png_buf);
    return false;
  }

  auto black = lv_color_make(0, 0, 0);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      lv_color_t color = lv_img_buf_get_px_color(img, x, y, black);
      size_t idx = (size_t) x * 3;
      auto r = LV_COLOR_GET_R(color);
      auto g = LV_COLOR_GET_G(color);
      auto b = LV_COLOR_GET_B(color);
      rowbuf[idx + 0] = (r << 3) | (r ? 7 : 0);
      rowbuf[idx + 1] = (g << 2) | (g ? 3 : 0);
      rowbuf[idx + 2] = (b << 3) | (b ? 7 : 0);
    }
    rc = png_encoder.addLine(rowbuf);
    if (rc != PNG_SUCCESS) {
      ESP_LOGW(TAG, "Error adding line %u to PNG encoding = %d", (unsigned) y, rc);
      break;
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  size_t bytes_written = 0;
  if (rc == PNG_SUCCESS) {
    bytes_written = png_encoder.close();
  }

  my_lvgl_free(rowbuf);

  if (rc != PNG_SUCCESS || bytes_written == 0) {
    my_lvgl_free(png_buf);
    return false;
  }

  *out_buf = png_buf;
  *out_size = bytes_written;
  return true;
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
  if (this->png_mutex_ == nullptr) {
    this->png_mutex_ = xSemaphoreCreateMutex();
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
  uint8_t *copy = nullptr;
  size_t copy_size = 0;
  if (this->parent_->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->parent_->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (this->parent_->last_png_buf_ != nullptr && this->parent_->last_png_size_ > 0) {
        copy_size = this->parent_->last_png_size_;
        copy = (uint8_t *) my_lvgl_malloc(copy_size);
        if (copy != nullptr) {
          memcpy(copy, this->parent_->last_png_buf_, copy_size);
        }
      }
      xSemaphoreGive(this->parent_->png_mutex_);
    }
  }

  if (copy != nullptr && copy_size > 0) {
    httpd_req_t *req = *request;
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_send(req, reinterpret_cast<const char *>(copy), (ssize_t)copy_size);
    my_lvgl_free(copy);
    return;
  }

  if (this->parent_->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->parent_->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->parent_->capture_requested_ = true;
      xSemaphoreGive(this->parent_->png_mutex_);
    }
  } else {
    this->parent_->capture_requested_ = true;
  }
  request->send(202, "text/plain", "Capture queued; retry in a moment");
  return;
}

 

void ScreenshotComponent::loop() {
  if (!this->capture_requested_ || this->capture_in_progress_)
    return;

  this->capture_in_progress_ = true;
  this->capture_requested_ = false;

  lv_img_dsc_t *img = grab_lvgl_rgb565();
  if (!img) {
    ESP_LOGW(TAG, "Main-loop capture returned NULL image");
    this->capture_in_progress_ = false;
    return;
  }

  uint8_t *png_buf = nullptr;
  size_t png_size = 0;
  bool ok = encode_png_to_buffer(img, &png_buf, &png_size);

  my_lvgl_free((void *) img->data);
  my_lvgl_free(img);

  if (!ok) {
    ESP_LOGW(TAG, "PNG encode failed; no cached image updated");
    this->capture_in_progress_ = false;
    return;
  }

  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (this->last_png_buf_ != nullptr) {
        my_lvgl_free(this->last_png_buf_);
      }
      this->last_png_buf_ = png_buf;
      this->last_png_size_ = png_size;
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    if (this->last_png_buf_ != nullptr) {
      my_lvgl_free(this->last_png_buf_);
    }
    this->last_png_buf_ = png_buf;
    this->last_png_size_ = png_size;
  }

#if HAVE_SD_MMC_CARD
  if (this->sd_mmc_card_ != nullptr) {
    const std::string path = "/sdcard/shot.png";
    bool mounted = this->sd_mmc_card_->is_mounted();
    if (!mounted) {
      mounted = this->sd_mmc_card_->mount();
    }
    if (!mounted) {
      ESP_LOGW(TAG, "SD mount failed; skipping write");
    } else {
      this->sd_mmc_card_->delete_file(path);
      bool wrote = this->sd_mmc_card_->append_file_chunk(path, png_buf, png_size, true);
      if (!wrote) {
        ESP_LOGW(TAG, "SD write failed: %s", path.c_str());
      } else {
        ESP_LOGD(TAG, "Wrote PNG to %s", path.c_str());
      }
    }
  } else {
    ESP_LOGD(TAG, "sd_mmc_card not configured, skipping SD write");
  }
#else
  ESP_LOGD(TAG, "sd_mmc_card support not compiled, skipping SD write");
#endif

  ESP_LOGD(TAG, "PNG image cached, %u bytes", (unsigned) png_size);
  this->capture_in_progress_ = false;
}

}  // namespace screenshot
}  // namespace esphome

#endif  // USE_ESP32
