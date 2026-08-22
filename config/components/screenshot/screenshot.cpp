
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

#if defined(__has_include)
#  if __has_include("esphome/components/sd_spi_card/sd_spi_card.h")
#    include "esphome/components/sd_spi_card/sd_spi_card.h"
#    define HAVE_SD_SPI_CARD 1
#  endif
#endif
#ifndef HAVE_SD_SPI_CARD
#  define HAVE_SD_SPI_CARD 0
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
#include <esp_timer.h>
#include <time.h>
#include <vector>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
// JPEG encoder (ESP32-P4 hardware accelerated)
#if defined(__has_include)
#  if __has_include("driver/jpeg_encode.h")
#    include "driver/jpeg_encode.h"
#    define HAVE_HW_JPEG_ENCODER 1
#  endif
#endif
#ifndef HAVE_HW_JPEG_ENCODER
#  define HAVE_HW_JPEG_ENCODER 0
#endif
// JPEG encoder removed; keep PPM path only

// libpng streaming removed; use zlib-based writer or stb fallback instead

// Try to use zlib (deflate) for on-the-fly IDAT generation
#if defined(__has_include)
#  if __has_include(<zlib.h>)
#    include <zlib.h>
#    define HAVE_ZLIB 1
#  endif
#endif

// --- Hardware JPEG helper (ESP32-P4) -------------------------------------
#if HAVE_HW_JPEG_ENCODER
static bool encode_raw_rgb565_to_jpeg(const uint8_t *src, size_t src_size, uint16_t width, uint16_t height,
                                      uint8_t **out_buf, size_t *out_size, uint8_t quality = 80, uint16_t timeout_ms = 200) {
  if (!src || src_size == 0 || !out_buf || !out_size) return false;

  jpeg_encode_engine_cfg_t eng_cfg = {
      .intr_priority = 0,
      .timeout_ms = timeout_ms,
  };
  jpeg_encoder_handle_t handle = NULL;
  esp_err_t err = jpeg_new_encoder_engine(&eng_cfg, &handle);
  if (err != ESP_OK) return false;

  size_t out_guess = src_size;
  jpeg_encode_memory_alloc_cfg_t mem_cfg = {
      .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
  };
  size_t actual_size = 0;
  uint8_t *buf = (uint8_t *)jpeg_alloc_encoder_mem(out_guess, &mem_cfg, &actual_size);
  if (!buf) {
    jpeg_del_encoder_engine(handle);
    return false;
  }

    jpeg_encode_cfg_t cfg = {
      .height = height,
      .width = width,
      .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
      .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
      .image_quality = quality,
    };

  uint32_t bytes_written = 0;
  err = jpeg_encoder_process(handle, &cfg, (uint8_t *)src, src_size, buf, actual_size ? actual_size : out_guess, &bytes_written);
  jpeg_del_encoder_engine(handle);
  if (err != ESP_OK) {
    free(buf);
    return false;
  }

  *out_buf = buf;
  *out_size = bytes_written;
  return true;
}
#endif
// --------------------------------------------------------------------------

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

namespace esphome {
namespace screenshot {

static const char *TAG = "screenshot";

static std::string get_query_string(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0) return std::string();
  std::string query;
  query.resize(qlen + 1);
  if (httpd_req_get_url_query_str(req, &query[0], query.size()) == ESP_OK) {
    if (!query.empty() && query.back() == '\0') query.pop_back();
    return query;
  }
  return std::string();
}

static bool query_has_key(const std::string &query, const std::string &key) {
  if (query.empty()) return false;
  if (query == key) return true;
  if (query.find(key + "=") != std::string::npos) return true;
  if (query.find(key + "&") != std::string::npos) return true;
  if (query.find("&" + key + "=") != std::string::npos) return true;
  if (query.find("&" + key + "&") != std::string::npos) return true;
  return false;
}

static bool query_get_u32(const std::string &query, const std::string &key, uint32_t *out) {
  if (out == nullptr || query.empty() || key.empty()) return false;

  std::string needle = key + "=";
  size_t pos = query.find(needle);
  if (pos == std::string::npos) {
    pos = query.find("&" + needle);
    if (pos == std::string::npos) return false;
    pos += 1;  // skip '&'
  }

  size_t val_start = pos + needle.size();
  size_t val_end = query.find('&', val_start);
  if (val_end == std::string::npos) val_end = query.size();
  if (val_end <= val_start) return false;

  uint32_t value = 0;
  for (size_t i = val_start; i < val_end; i++) {
    char c = query[i];
    if (c < '0' || c > '9') return false;
    value = value * 10U + static_cast<uint32_t>(c - '0');
  }

  *out = value;
  return true;
}

// When using LVGL snapshot API we can avoid copying by using the image's data
// pointer and freeing the lv_img_dsc_t when done. Store the last snapshot here.
// (no fallback contiguous buffer)


// Helper: grab LVGL active screen buffer pointer and size. This is best-effort.
static lv_draw_buf_t *grab_lvgl_rgb565() {
  lv_obj_t *scr = lv_scr_act();
  ESP_LOGD(TAG, "grab_lvgl_rgb565: lv_scr_act -> %p", (void *)scr);
  /* Use LVGL's native snapshot allocator and capture path. */
  lv_refr_now(NULL);
  lv_draw_buf_t *draw_buf = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
  if (draw_buf == nullptr) {
    ESP_LOGW(TAG, "grab_lvgl_rgb565: lv_snapshot_take failed");
  }
  return draw_buf;
}

static bool encode_png_to_buffer(const lv_draw_buf_t *img, uint8_t **out_buf, size_t *out_size) {
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

  const auto *pixels = reinterpret_cast<const lv_color16_t *>(img->data);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      lv_color16_t color = pixels[(size_t) y * w + x];
      size_t idx = (size_t) x * 3;
      auto r = color.red;
      auto g = color.green;
      auto b = color.blue;
      rowbuf[idx + 0] = (r << 3) | (r ? 7 : 0);
      rowbuf[idx + 1] = (g << 2) | (g ? 3 : 0);
      rowbuf[idx + 2] = (b << 3) | (b ? 7 : 0);
    }
    rc = png_encoder.addLine(rowbuf);
    if (rc != PNG_SUCCESS) {
      ESP_LOGW(TAG, "Error adding line %u to PNG encoding = %d", (unsigned) y, rc);
      break;
    }
    App.feed_wdt();
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
#if HAVE_CAMERA
  // Register /capture handler for camera captures
  this->capture_handler_ = new CaptureHandler(this);
  web_server_base::global_web_server_base->add_handler(this->capture_handler_);
  // Register /snapshot.jpg handler — synchronous high-res JPEG delivery
  this->snapshot_handler_ = new SnapshotHandler(this);
  web_server_base::global_web_server_base->add_handler(this->snapshot_handler_);
  // Register /video handler — MJPEG live stream
  this->video_handler_ = new VideoHandler(this);
  web_server_base::global_web_server_base->add_handler(this->video_handler_);
#endif
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
  if (this->snapshot_done_ == nullptr) {
    this->snapshot_done_ = xSemaphoreCreateBinary();
  }
  ESP_LOGI(TAG, "Registered /screenshot.png, /snapshot.jpg and /video handlers with web_server");
}

void ScreenshotComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Screenshot endpoint: /screenshot.jpg (via web_server)");
}

// Forward declaration for synchronous handler helper
static void process_request(AsyncWebServerRequest *request);

void ScreenshotComponent::Handler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI(TAG, "HTTP /screenshot.png request via web_server");
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  ESP_LOGD(TAG, "Request URL='%s' Method=%d", request->url_to(url_buf).c_str(), request->method());
  std::string query = get_query_string(request);

  if (query_has_key(query, "status")) {
    bool ready = false;
    bool saved = false;
    bool in_progress = false;
    uint32_t last_save_epoch = 0;
    uint32_t last_capture_epoch = 0;
    std::string last_path;
    if (this->parent_->png_mutex_ != nullptr) {
      if (xSemaphoreTake(this->parent_->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        ready = this->parent_->last_png_buf_ != nullptr && this->parent_->last_png_size_ > 0;
        saved = this->parent_->last_save_ok_;
        last_save_epoch = this->parent_->last_save_epoch_;
        last_capture_epoch = this->parent_->last_capture_epoch_;
        last_path = this->parent_->last_save_path_;
        in_progress = this->parent_->capture_in_progress_;
        xSemaphoreGive(this->parent_->png_mutex_);
      }
    } else {
      ready = this->parent_->last_png_buf_ != nullptr && this->parent_->last_png_size_ > 0;
      saved = this->parent_->last_save_ok_;
      last_save_epoch = this->parent_->last_save_epoch_;
      last_capture_epoch = this->parent_->last_capture_epoch_;
      last_path = this->parent_->last_save_path_;
      in_progress = this->parent_->capture_in_progress_;
    }

    char body[256];
    snprintf(body, sizeof(body),
             "{ \"ready\": %s, \"in_progress\": %s, \"saved\": %s, \"last_save_epoch\": %u, "
             "\"last_capture_epoch\": %u, \"last_path\": \"%s\" }",
             ready ? "true" : "false", in_progress ? "true" : "false", saved ? "true" : "false",
             (unsigned) last_save_epoch, (unsigned) last_capture_epoch, last_path.c_str());
    request->send(200, "application/json", body);
    return;
  }

  uint8_t *copy = nullptr;
  size_t copy_size = 0;
  bool saved = false;
  uint32_t last_save_epoch = 0;
  std::string last_path;
  if (this->parent_->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->parent_->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (this->parent_->last_png_buf_ != nullptr && this->parent_->last_png_size_ > 0) {
        copy_size = this->parent_->last_png_size_;
        copy = (uint8_t *) my_lvgl_malloc(copy_size);
        if (copy != nullptr) {
          memcpy(copy, this->parent_->last_png_buf_, copy_size);
        }
      }
      saved = this->parent_->last_save_ok_;
      last_save_epoch = this->parent_->last_save_epoch_;
      last_path = this->parent_->last_save_path_;
      xSemaphoreGive(this->parent_->png_mutex_);
    }
  }

  if (copy != nullptr && copy_size > 0) {
    this->parent_->save_requested_ = true;
    if (this->parent_->png_mutex_ != nullptr) {
      if (xSemaphoreTake(this->parent_->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (this->parent_->last_png_buf_ != nullptr) {
          my_lvgl_free(this->parent_->last_png_buf_);
        }
        this->parent_->last_png_buf_ = nullptr;
        this->parent_->last_png_size_ = 0;
        xSemaphoreGive(this->parent_->png_mutex_);
      }
    } else {
      if (this->parent_->last_png_buf_ != nullptr) {
        my_lvgl_free(this->parent_->last_png_buf_);
      }
      this->parent_->last_png_buf_ = nullptr;
      this->parent_->last_png_size_ = 0;
    }
    httpd_req_t *req = *request;
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "X-Screenshot-Ready", "1");
    httpd_resp_set_hdr(req, "X-Screenshot-Saved", saved ? "1" : "0");
    if (!last_path.empty()) {
      httpd_resp_set_hdr(req, "X-Screenshot-Path", last_path.c_str());
    }
    char epoch_buf[16];
    snprintf(epoch_buf, sizeof(epoch_buf), "%u", (unsigned) last_save_epoch);
    httpd_resp_set_hdr(req, "X-Screenshot-Save-Epoch", epoch_buf);
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
  request->send(202, "application/json",
                "{ \"ready\": false, \"in_progress\": true, \"saved\": false, "
                "\"message\": \"Capture queued; retry in a moment\" }");
  return;
}
#if HAVE_CAMERA
void ScreenshotComponent::CaptureHandler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI(TAG, "HTTP /capture request via web_server");
  std::string query = get_query_string(request);
  // Queue a camera capture for the main loop to process
  if (this->parent_->camera_mutex_ != nullptr) {
    if (xSemaphoreTake(this->parent_->camera_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->parent_->camera_capture_requested_ = true;
      xSemaphoreGive(this->parent_->camera_mutex_);
    }
  } else {
    this->parent_->camera_capture_requested_ = true;
  }

  request->send(202, "application/json",
                "{ \"ready\": false, \"in_progress\": true, \"message\": \"Camera capture queued; check /capture?status\" }");
}

void ScreenshotComponent::SnapshotHandler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI(TAG, "HTTP /snapshot.jpg request");

  // Reject concurrent snapshots
  if (this->parent_->snapshot_in_progress_) {
    request->send(503, "text/plain", "Snapshot already in progress; please retry.");
    return;
  }
  if (this->parent_->camera_ == nullptr) {
    request->send(503, "text/plain", "No camera configured.");
    return;
  }

  // Reset result state and queue capture on main loop
  this->parent_->snapshot_jpeg_buf_ = nullptr;
  this->parent_->snapshot_jpeg_size_ = 0;
  this->parent_->snapshot_in_progress_ = true;
  this->parent_->snapshot_requested_ = true;

  // Block until loop() signals completion (up to 10 s: 2 s frame-wait + encode)
  bool got_sem = (this->parent_->snapshot_done_ != nullptr) &&
                 (xSemaphoreTake(this->parent_->snapshot_done_, pdMS_TO_TICKS(10000)) == pdTRUE);

  if (got_sem && this->parent_->snapshot_jpeg_buf_ != nullptr && this->parent_->snapshot_jpeg_size_ > 0) {
    // Send JPEG directly to client using low-level httpd API
    httpd_req_t *req = *request;
    char len_buf[24];
    snprintf(len_buf, sizeof(len_buf), "%u", (unsigned)this->parent_->snapshot_jpeg_size_);
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"snapshot.jpg\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, reinterpret_cast<const char *>(this->parent_->snapshot_jpeg_buf_),
                    (ssize_t)this->parent_->snapshot_jpeg_size_);
    // Free buffer now that it has been sent
    free(this->parent_->snapshot_jpeg_buf_);
    this->parent_->snapshot_jpeg_buf_ = nullptr;
    this->parent_->snapshot_jpeg_size_ = 0;
    ESP_LOGI(TAG, "/snapshot.jpg: sent %s bytes", len_buf);
  } else if (!got_sem) {
    // loop() never signaled within 20 s
    this->parent_->snapshot_requested_ = false;  // cancel pending request
    request->send(503, "text/plain", "Snapshot timed out (no frame within 20 s).");
  } else {
    // loop() ran but capture failed
    request->send(500, "text/plain", "Snapshot capture failed.");
  }

  this->parent_->snapshot_in_progress_ = false;
}

// ---------------------------------------------------------------------------
// /video  —  MJPEG live stream (multipart/x-mixed-replace)
// ---------------------------------------------------------------------------
void ScreenshotComponent::VideoHandler::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGI(TAG, "HTTP /video request");

  // /video behaves as a finite endpoint by default.
  // Optional query params:
  //   max_frames=<n>   (default: 120)
  //   max_ms=<n>       (default: 15000)
  //   continuous=1     (disable endpoint limits; legacy behavior)
  std::string query = get_query_string(request);
  uint32_t max_frames = 120;
  uint32_t max_stream_ms = 15000;
  bool continuous = query_has_key(query, "continuous");
  uint32_t tmp = 0;
  if (query_get_u32(query, "max_frames", &tmp) && tmp > 0) max_frames = tmp;
  if (query_get_u32(query, "max_ms", &tmp) && tmp > 0) max_stream_ms = tmp;

  if (this->parent_->camera_ == nullptr) {
    request->send(503, "text/plain", "No camera configured.");
    return;
  }
  // Reject second concurrent client
  if (this->parent_->video_streaming_) {
    request->send(503, "text/plain", "Video stream already active; only one client supported.");
    return;
  }
  this->parent_->video_streaming_ = true;

  httpd_req_t *req = *request;

  // MJPEG stream headers
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Connection", "close");

  auto cam = this->parent_->camera_;
  bool was_streaming = cam->is_streaming();
  bool started_by_video = false;
  if (!was_streaming) {
    started_by_video = cam->start_streaming();
    vTaskDelay(pdMS_TO_TICKS(200));  // allow sensor to produce first frames
  }

  ESP_LOGI(TAG, "/video: streaming %ux%u MJPEG to client",
           (unsigned)cam->get_image_width(), (unsigned)cam->get_image_height());
  const uint32_t stream_start = millis();

  uint32_t frames_sent = 0;
  unsigned consecutive_errors = 0;

  while (consecutive_errors < 8) {
    if (!continuous) {
      if ((millis() - stream_start) >= max_stream_ms) {
        ESP_LOGI(TAG, "/video: endpoint complete after %u ms", (unsigned)max_stream_ms);
        break;
      }
      if (frames_sent >= max_frames) {
        ESP_LOGI(TAG, "/video: endpoint complete after %u frames", (unsigned)max_frames);
        break;
      }
    }

    // Wait for next frame (up to 1 s at 30 fps)
    uint32_t t0 = millis();
    bool got = false;
    while ((millis() - t0) < 1000) {
      if (cam->capture_frame()) { got = true; break; }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!got) {
      consecutive_errors++;
      ESP_LOGW(TAG, "/video: no frame within 1 s (errors=%u)", consecutive_errors);
      continue;
    }
    consecutive_errors = 0;

    uint8_t *img = cam->get_image_data();
    size_t isz   = cam->get_image_size();
    uint16_t w   = cam->get_image_width();
    uint16_t h   = cam->get_image_height();

    if (!img || isz == 0) continue;

    uint8_t *jpeg_buf = nullptr;
    size_t   jpeg_size = 0;
    bool     owned = false;

    if (isz >= 2 && img[0] == 0xFF && img[1] == 0xD8) {
      // Sensor delivered native JPEG
      jpeg_buf  = img;
      jpeg_size = isz;
    } else {
#if HAVE_HW_JPEG_ENCODER
      if (!encode_raw_rgb565_to_jpeg(img, isz, w, h, &jpeg_buf, &jpeg_size, 80, 300)) {
        ESP_LOGW(TAG, "/video: JPEG encode failed");
        continue;
      }
      owned = true;
#else
      ESP_LOGW(TAG, "/video: no HW JPEG encoder, cannot stream");
      break;
#endif
    }

    // Build MJPEG frame header
    char hdr[128];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "--frame\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %u\r\n"
        "\r\n",
        (unsigned)jpeg_size);

    esp_err_t err = httpd_resp_send_chunk(req, hdr, hdr_len);
    if (err == ESP_OK)
      err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(jpeg_buf), (ssize_t)jpeg_size);
    if (err == ESP_OK)
      err = httpd_resp_send_chunk(req, "\r\n", 2);

    if (owned) free(jpeg_buf);

    if (err != ESP_OK) {
      // Client disconnected
      ESP_LOGI(TAG, "/video: client disconnected after %u frames", frames_sent);
      break;
    }

    frames_sent++;
    if (frames_sent % 100 == 0)
      ESP_LOGI(TAG, "/video: %u frames streamed", frames_sent);

    App.feed_wdt();
  }

  // End chunked transfer
  httpd_resp_send_chunk(req, nullptr, 0);

  // Stop camera only if this /video request started it.
  // If streaming was already active before, keep previous state untouched.
  if (started_by_video) {
    cam->stop_streaming();
  }
  this->parent_->video_streaming_ = false;
  ESP_LOGI(TAG, "/video: stream ended (%u frames total, was_streaming=%d, continuous=%d)",
           frames_sent, was_streaming ? 1 : 0, continuous ? 1 : 0);
}
#endif  // HAVE_CAMERA

bool ScreenshotComponent::write_png_to_sd_(const uint8_t *png_buf, size_t png_size) {
#if HAVE_SD_MMC_CARD || HAVE_SD_SPI_CARD
  if (png_buf == nullptr || png_size == 0) return false;
  char ts_buf[32] = {0};
  bool have_wall_time = false;
  ::time_t now = ::time(nullptr);
  if (now > 0) {
    ::tm lt;
    if (::localtime_r(&now, &lt) != nullptr) {
      if (lt.tm_year >= 120) {  // year >= 2020
        ::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &lt);
        have_wall_time = ts_buf[0] != '\0';
      }
    }
  }
  if (!have_wall_time) {
    const uint64_t now_us = esp_timer_get_time();
    const unsigned long long now_s = static_cast<unsigned long long>(now_us / 1000000ULL);
    snprintf(ts_buf, sizeof(ts_buf), "uptime_%llus", now_s);
  }

  char safe_ts[32] = {0};
  size_t safe_len = 0;
  for (size_t i = 0; ts_buf[i] != '\0' && safe_len + 1 < sizeof(safe_ts); ++i) {
    char c = ts_buf[i];
    if (c == ':') c = '-';
    if (c == ' ') c = '_';
    safe_ts[safe_len++] = c;
  }
  safe_ts[safe_len] = '\0';

  char path_buf[96];
  snprintf(path_buf, sizeof(path_buf), "/sdcard/screenshot_%s.png", safe_ts);
  const std::string path = path_buf;
  bool wrote = false;
  bool attempted = false;
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    attempted = true;
    bool mounted = this->sd_spi_card_->is_mounted();
    if (!mounted) {
      mounted = this->sd_spi_card_->mount();
    }
    if (!mounted) {
      ESP_LOGW(TAG, "SD SPI mount failed; skipping write");
      wrote = false;
    } else {
      this->sd_spi_card_->delete_file(path);
      wrote = this->sd_spi_card_->append_file_chunk(path, png_buf, png_size, true);
      if (!wrote) {
        ESP_LOGW(TAG, "SD SPI write failed: %s", path.c_str());
      } else {
        ESP_LOGD(TAG, "Wrote PNG to %s", path.c_str());
      }
    }
  }
#endif
  if (!attempted && this->sd_mmc_card_ != nullptr) {
    attempted = true;
    this->sd_mmc_card_->delete_file(path);
    this->sd_mmc_card_->append_file(path.c_str(), png_buf, png_size);
    wrote = true;
    ESP_LOGD(TAG, "Wrote PNG to %s", path.c_str());
  }
  if (!attempted) {
    ESP_LOGD(TAG, "sd card not configured, skipping SD write");
    wrote = false;
  }

  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->last_save_ok_ = wrote;
      this->last_save_path_ = wrote ? path : "";
      this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    this->last_save_ok_ = wrote;
    this->last_save_path_ = wrote ? path : "";
    this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }
  return wrote;
#else
  ESP_LOGD(TAG, "sd card support not compiled, skipping SD write");
  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->last_save_ok_ = false;
      this->last_save_path_.clear();
      this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    this->last_save_ok_ = false;
    this->last_save_path_.clear();
    this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }
  return false;
#endif
}
#if HAVE_CAMERA  
bool ScreenshotComponent::write_camera_png_to_sd_(const uint8_t *rgb565_buf, uint16_t width, uint16_t height) {
  if (!rgb565_buf || width == 0 || height == 0) return false;
  // Convert RGB565 to RGB888 per-row and use PNGenc similar to encode_png_to_buffer
  size_t row_bytes = (size_t)width * 3;
  uint8_t *rowbuf = (uint8_t *) my_lvgl_malloc(row_bytes);
  if (!rowbuf) {
    ESP_LOGW(TAG, "write_camera_png_to_sd_: failed to allocate row buffer %u", (unsigned) row_bytes);
    return false;
  }

  size_t png_bytes = (size_t) width * (size_t) height;
  uint8_t *png_buf = (uint8_t *) my_lvgl_malloc(png_bytes);
  if (!png_buf) {
    ESP_LOGW(TAG, "write_camera_png_to_sd_: failed to allocate png buffer %u", (unsigned) png_bytes);
    my_lvgl_free(rowbuf);
    return false;
  }

  PNGenc png_encoder;
  int rc = png_encoder.open(png_buf, png_bytes);
  if (rc != PNG_SUCCESS) {
    ESP_LOGW(TAG, "write_camera_png_to_sd_: PNG open failed %d", rc);
    my_lvgl_free(rowbuf);
    my_lvgl_free(png_buf);
    return false;
  }
  rc = png_encoder.encodeBegin(width, height, PNG_PIXEL_TRUECOLOR, 24, NULL, 1);
  if (rc != PNG_SUCCESS) {
    ESP_LOGW(TAG, "write_camera_png_to_sd_: PNG encodeBegin failed %d", rc);
    my_lvgl_free(rowbuf);
    my_lvgl_free(png_buf);
    return false;
  }

  // rgb565_buf is assumed row-major, 2 bytes per pixel
  const uint16_t *src = reinterpret_cast<const uint16_t *>(rgb565_buf);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint16_t pix = src[y * width + x];
      uint8_t r = ((pix >> 11) & 0x1F) << 3;
      uint8_t g = ((pix >> 5) & 0x3F) << 2;
      uint8_t b = (pix & 0x1F) << 3;
      size_t idx = (size_t)x * 3;
      rowbuf[idx + 0] = r ? r : 0;
      rowbuf[idx + 1] = g ? g : 0;
      rowbuf[idx + 2] = b ? b : 0;
    }
    rc = png_encoder.addLine(rowbuf);
    if (rc != PNG_SUCCESS) {
      ESP_LOGW(TAG, "write_camera_png_to_sd_: PNG addLine failed y=%u rc=%d", (unsigned)y, rc);
      break;
    }
    App.feed_wdt();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  size_t bytes_written = 0;
  if (rc == PNG_SUCCESS) bytes_written = png_encoder.close();
  my_lvgl_free(rowbuf);

  if (rc != PNG_SUCCESS || bytes_written == 0) {
    my_lvgl_free(png_buf);
    return false;
  }

  bool ok = this->write_png_to_sd_(png_buf, bytes_written);
  // write_png_to_sd_ will copy path info into last_save_* fields
  // png_buf is owned by write_png_to_sd_ call? it expects to be provided as a buffer; our helper used my_lvgl_malloc
  // But write_png_to_sd_ will not free png_buf; we must free after write
  my_lvgl_free(png_buf);
  return ok;
}

bool ScreenshotComponent::write_camera_jpeg_to_sd_(const uint8_t *jpeg_buf, size_t jpeg_size) {
  if (!jpeg_buf || jpeg_size == 0) return false;
#if HAVE_SD_MMC_CARD || HAVE_SD_SPI_CARD
  char ts_buf[32] = {0};
  bool have_wall_time = false;
  ::time_t now = ::time(nullptr);
  if (now > 0) {
    ::tm lt;
    if (::localtime_r(&now, &lt) != nullptr) {
      if (lt.tm_year >= 120) {
        ::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &lt);
        have_wall_time = ts_buf[0] != '\0';
      }
    }
  }
  if (!have_wall_time) {
    const uint64_t now_us = esp_timer_get_time();
    const unsigned long long now_s = static_cast<unsigned long long>(now_us / 1000000ULL);
    snprintf(ts_buf, sizeof(ts_buf), "uptime_%llus", now_s);
  }

  char safe_ts[32] = {0};
  size_t safe_len = 0;
  for (size_t i = 0; ts_buf[i] != '\0' && safe_len + 1 < sizeof(safe_ts); ++i) {
    char c = ts_buf[i];
    if (c == ':') c = '-';
    if (c == ' ') c = '_';
    safe_ts[safe_len++] = c;
  }
  safe_ts[safe_len] = '\0';

  char path_buf[96];
  snprintf(path_buf, sizeof(path_buf), "/sdcard/capture_%s.jpg", safe_ts);
  const std::string path = path_buf;
  bool wrote = false;
  bool attempted = false;
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    attempted = true;
    bool mounted = this->sd_spi_card_->is_mounted();
    if (!mounted) mounted = this->sd_spi_card_->mount();
    if (!mounted) {
      ESP_LOGW(TAG, "SD SPI mount failed; skipping write");
      wrote = false;
    } else {
      this->sd_spi_card_->delete_file(path);
      wrote = this->sd_spi_card_->append_file_chunk(path, jpeg_buf, jpeg_size, true);
      if (!wrote) ESP_LOGW(TAG, "SD SPI write failed: %s", path.c_str());
      else ESP_LOGD(TAG, "Wrote JPEG to %s", path.c_str());
    }
  }
#endif
  if (!attempted && this->sd_mmc_card_ != nullptr) {
    attempted = true;
    this->sd_mmc_card_->delete_file(path);
    this->sd_mmc_card_->append_file(path.c_str(), jpeg_buf, jpeg_size);
    wrote = true;
    ESP_LOGD(TAG, "Wrote JPEG to %s", path.c_str());
  }
  if (!attempted) {
    ESP_LOGD(TAG, "sd card not configured, skipping SD write");
    wrote = false;
  }

  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->last_save_ok_ = wrote;
      this->last_save_path_ = wrote ? path : "";
      this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    this->last_save_ok_ = wrote;
    this->last_save_path_ = wrote ? path : "";
    this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }
  return wrote;
#else
  ESP_LOGD(TAG, "sd card support not compiled, skipping SD write");
  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->last_save_ok_ = false;
      this->last_save_path_.clear();
      this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    this->last_save_ok_ = false;
    this->last_save_path_.clear();
    this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }
  return false;
#endif
}

bool ScreenshotComponent::write_snapshot_jpeg_to_sd_(const uint8_t *jpeg_buf, size_t jpeg_size) {
  if (!jpeg_buf || jpeg_size == 0) return false;
#if HAVE_SD_MMC_CARD || HAVE_SD_SPI_CARD
  char ts_buf[32] = {0};
  bool have_wall_time = false;
  ::time_t now = ::time(nullptr);
  if (now > 0) {
    ::tm lt;
    if (::localtime_r(&now, &lt) != nullptr) {
      if (lt.tm_year >= 120) {
        ::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &lt);
        have_wall_time = ts_buf[0] != '\0';
      }
    }
  }
  if (!have_wall_time) {
    const uint64_t now_us = esp_timer_get_time();
    const unsigned long long now_s = static_cast<unsigned long long>(now_us / 1000000ULL);
    snprintf(ts_buf, sizeof(ts_buf), "uptime_%llus", now_s);
  }

  char safe_ts[32] = {0};
  size_t safe_len = 0;
  for (size_t i = 0; ts_buf[i] != '\0' && safe_len + 1 < sizeof(safe_ts); ++i) {
    char c = ts_buf[i];
    if (c == ':') c = '-';
    if (c == ' ') c = '_';
    safe_ts[safe_len++] = c;
  }
  safe_ts[safe_len] = '\0';

  char path_buf[96];
  snprintf(path_buf, sizeof(path_buf), "/sdcard/screenshot_%s.jpg", safe_ts);
  const std::string path = path_buf;
  bool wrote = false;
  bool attempted = false;
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    attempted = true;
    bool mounted = this->sd_spi_card_->is_mounted();
    if (!mounted) mounted = this->sd_spi_card_->mount();
    if (!mounted) {
      ESP_LOGW(TAG, "SD SPI mount failed; skipping write");
      wrote = false;
    } else {
      this->sd_spi_card_->delete_file(path);
      wrote = this->sd_spi_card_->append_file_chunk(path, jpeg_buf, jpeg_size, true);
      if (!wrote) ESP_LOGW(TAG, "SD SPI write failed: %s", path.c_str());
      else ESP_LOGD(TAG, "Wrote snapshot JPEG to %s", path.c_str());
    }
  }
#endif
  if (!attempted && this->sd_mmc_card_ != nullptr) {
    attempted = true;
    this->sd_mmc_card_->delete_file(path);
    this->sd_mmc_card_->append_file(path.c_str(), jpeg_buf, jpeg_size);
    wrote = true;
    ESP_LOGD(TAG, "Wrote snapshot JPEG to %s", path.c_str());
  }
  if (!attempted) {
    ESP_LOGD(TAG, "sd card not configured, skipping SD write");
    wrote = false;
  }

  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->last_save_ok_ = wrote;
      this->last_save_path_ = wrote ? path : "";
      this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    this->last_save_ok_ = wrote;
    this->last_save_path_ = wrote ? path : "";
    this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }
  return wrote;
#else
  ESP_LOGD(TAG, "sd card support not compiled, skipping SD write");
  if (this->png_mutex_ != nullptr) {
    if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      this->last_save_ok_ = false;
      this->last_save_path_.clear();
      this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    this->last_save_ok_ = false;
    this->last_save_path_.clear();
    this->last_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }
  return false;
#endif
}
#endif

 

void ScreenshotComponent::loop() {
  if (this->save_requested_ && !this->capture_in_progress_ && !this->capture_requested_) {
    uint8_t *buf = nullptr;
    size_t size = 0;
    if (this->png_mutex_ != nullptr) {
      if (xSemaphoreTake(this->png_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
        buf = this->last_png_buf_;
        size = this->last_png_size_;
        xSemaphoreGive(this->png_mutex_);
      }
    } else {
      buf = this->last_png_buf_;
      size = this->last_png_size_;
    }
    if (buf != nullptr && size > 0) {
      this->write_png_to_sd_(buf, size);
    }
    this->save_requested_ = false;
  } 


#if HAVE_CAMERA
    // Camera capture flow: stop streaming, switch to QSXGA, capture, revert to SVGA
    if (this->camera_capture_requested_ && !this->camera_capture_in_progress_) {
      this->camera_capture_in_progress_ = true;
      this->camera_capture_requested_ = false;
      ESP_LOGI(TAG, "Starting camera capture task");
      if (this->camera_ != nullptr) {
        auto cam = this->camera_;
        bool was_streaming = cam->is_streaming();
        ESP_LOGD(TAG, "Camera was_streaming=%d before capture", was_streaming ? 1 : 0);
        if (was_streaming) cam->stop_streaming();

        // Switch to FHD (1920×1080 RAW10) using official Espressif OV5647 register table
        ESP_LOGD(TAG, "Requesting camera resolution FHD (1920x1080 RAW10) for capture");
        cam->set_pixel_format(::esphome::tab5_camera::PIXEL_FORMAT_RGB565);
        cam->set_resolution(::esphome::tab5_camera::RESOLUTION_FHD);
        cam->reconfigure_resolution(::esphome::tab5_camera::RESOLUTION_FHD);

        // Log immediate config state
        ESP_LOGD(TAG, "Post-reconfigure (pre-start): image_size=%u width=%u height=%u",
                 (unsigned)cam->get_image_size(), (unsigned)cam->get_image_width(), (unsigned)cam->get_image_height());

        // Start streaming to get a fresh frame (driver may provide compressed JPEG)
        cam->start_streaming();
        ESP_LOGD(TAG, "After start_streaming: image_size=%u width=%u height=%u",
           (unsigned)cam->get_image_size(), (unsigned)cam->get_image_width(), (unsigned)cam->get_image_height());

        ESP_LOGD(TAG, "Camera reconfigured to FHD 1920x1080 RAW10@30fps");
        // Wait for a new frame (timeout 15s)
        uint32_t start_ms = millis();
        bool got = false;
        unsigned attempts = 0;
        while ((millis() - start_ms) < 15000) {
          ++attempts;
          if (cam->capture_frame()) { got = true; break; }
          if ((attempts & 0x1F) == 0) { // every 32 attempts (~640ms) log a heartbeat
            ESP_LOGD(TAG, "capture_frame still false after %u attempts; image_size=%u width=%u height=%u",
                     attempts, (unsigned)cam->get_image_size(), (unsigned)cam->get_image_width(), (unsigned)cam->get_image_height());
          }
          App.feed_wdt();
          vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (got) {
          uint8_t *imgbuf = cam->get_image_data();
          size_t img_size = cam->get_image_size();
          bool wrote = false;
          if (imgbuf != nullptr && img_size > 0) {
            // Quick heuristic: check for JPEG SOI 0xFF 0xD8
            if (img_size >= 2 && imgbuf[0] == 0xFF && imgbuf[1] == 0xD8) {
              ESP_LOGI(TAG, "Captured buffer appears to be JPEG (size=%u)", (unsigned)img_size);
              wrote = this->write_camera_jpeg_to_sd_(imgbuf, img_size);
            } else {
              uint16_t w = cam->get_image_width();
              uint16_t h = cam->get_image_height();
              ESP_LOGW(TAG, "Captured buffer missing JPEG SOI; treating as raw RGB565 %ux%u size=%u",
                       (unsigned)w, (unsigned)h, (unsigned)img_size);
              // Dump first bytes for diagnostics
              unsigned dump_len = img_size < 16 ? (unsigned)img_size : 16u;
              char hexbuf[64];
              size_t pos = 0;
              for (unsigned i = 0; i < dump_len && pos + 4 < sizeof(hexbuf); ++i) {
                pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02X ", imgbuf[i]);
              }
              ESP_LOGD(TAG, "First %u bytes: %s", dump_len, hexbuf);
              // Fallback: try hardware JPEG encode (ESP32-P4) then PNG
#if HAVE_HW_JPEG_ENCODER
              uint8_t *hw_jpeg = nullptr;
              size_t hw_jpeg_size = 0;
              if (encode_raw_rgb565_to_jpeg(imgbuf, img_size, w, h, &hw_jpeg, &hw_jpeg_size)) {
                ESP_LOGD(TAG, "On-device JPEG encode succeeded size=%u", (unsigned)hw_jpeg_size);
                wrote = this->write_camera_jpeg_to_sd_(hw_jpeg, hw_jpeg_size);
                free(hw_jpeg);
              } else {
                ESP_LOGW(TAG, "On-device JPEG encode failed; falling back to PNG");
                wrote = this->write_camera_png_to_sd_(imgbuf, cam->get_image_width(), cam->get_image_height());
              }
#else
              wrote = this->write_camera_png_to_sd_(imgbuf, cam->get_image_width(), cam->get_image_height());
#endif
            }
          } else {
            ESP_LOGW(TAG, "Captured frame buffer is null or empty (size=%u)", (unsigned)img_size);
          }
          if (this->camera_mutex_ != nullptr) {
            if (xSemaphoreTake(this->camera_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
              this->last_camera_save_ok_ = wrote;
              this->last_camera_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
              xSemaphoreGive(this->camera_mutex_);
            }
          } else {
            this->last_camera_save_ok_ = wrote;
            this->last_camera_save_epoch_ = static_cast<uint32_t>(::time(nullptr));
          }
        } else {
          ESP_LOGW(TAG, "Camera capture timed out after %u attempts (elapsed ms=%u); final image_size=%u width=%u height=%u",
                   attempts, (unsigned)(millis() - start_ms), (unsigned)cam->get_image_size(), (unsigned)cam->get_image_width(), (unsigned)cam->get_image_height());
        }
        // Stop and revert to previous streaming/resolution and pixel format
        cam->stop_streaming();
        cam->set_pixel_format(::esphome::tab5_camera::PIXEL_FORMAT_RGB565);
        cam->set_resolution(::esphome::tab5_camera::RESOLUTION_SVGA);
        cam->reconfigure_resolution(::esphome::tab5_camera::RESOLUTION_SVGA);
        if (was_streaming) cam->start_streaming();
      }

      this->camera_capture_in_progress_ = false;
    }

    // ---- /snapshot.jpg: capture current SVGA streaming frame -> JPEG ----
    // We no longer attempt a QSXGA resolution switch because the CSI/ISP
    // hardware cannot be reliably re-initialized at a different resolution at
    // runtime on this platform.  Instead we grab the next SVGA (800x640) RGB565
    // frame that the sensor is already delivering and encode it to JPEG using
    // the ESP32-P4 hardware JPEG accelerator.  At 50 fps a frame arrives every
    // ~20 ms, so the whole operation completes in well under 500 ms.
    if (this->snapshot_requested_) {
      this->snapshot_requested_ = false;
      this->snapshot_jpeg_buf_  = nullptr;
      this->snapshot_jpeg_size_ = 0;

      if (this->camera_ != nullptr) {
        auto cam = this->camera_;
        bool was_streaming = cam->is_streaming();

        // Ensure streaming is active
        if (!was_streaming) {
          cam->start_streaming();
          vTaskDelay(pdMS_TO_TICKS(150));  // allow sensor to produce first frame
        }

        ESP_LOGI(TAG, "/snapshot.jpg: grabbing SVGA %ux%u frame",
                 (unsigned)cam->get_image_width(), (unsigned)cam->get_image_height());

        // Wait up to 2 s for a fresh frame — allow extra time on first start
        // (30 fps = new frame every ~33 ms; 2 s budget handles sensor settle)
        uint32_t snap_start = millis();
        bool snap_got = false;
        while ((millis() - snap_start) < 2000) {
          if (cam->capture_frame()) { snap_got = true; break; }
          App.feed_wdt();
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (snap_got) {
          uint8_t *img  = cam->get_image_data();
          size_t   isz  = cam->get_image_size();
          uint16_t sw   = cam->get_image_width();
          uint16_t sh   = cam->get_image_height();

          if (img && isz > 0) {
            if (isz >= 2 && img[0] == 0xFF && img[1] == 0xD8) {
              // Sensor delivered native JPEG
              ESP_LOGI(TAG, "/snapshot.jpg: native JPEG %u bytes", (unsigned)isz);
              uint8_t *copy = (uint8_t *)heap_caps_malloc(isz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
              if (!copy) copy = (uint8_t *)malloc(isz);
              if (copy) { memcpy(copy, img, isz); this->snapshot_jpeg_buf_ = copy; this->snapshot_jpeg_size_ = isz; }
            } else {
              // RGB565 raw frame -> HW JPEG encode
              ESP_LOGI(TAG, "/snapshot.jpg: HW-encoding %ux%u RGB565 frame (%u bytes)",
                       (unsigned)sw, (unsigned)sh, (unsigned)isz);
#if HAVE_HW_JPEG_ENCODER
              encode_raw_rgb565_to_jpeg(img, isz, sw, sh,
                                        &this->snapshot_jpeg_buf_, &this->snapshot_jpeg_size_);
#else
              ESP_LOGW(TAG, "/snapshot.jpg: no HW JPEG encoder available");
#endif
            }

            if (this->snapshot_jpeg_buf_ != nullptr && this->snapshot_jpeg_size_ > 0) {
              this->write_snapshot_jpeg_to_sd_(this->snapshot_jpeg_buf_, this->snapshot_jpeg_size_);
            }
          }
        } else {
          ESP_LOGW(TAG, "/snapshot.jpg: no frame received within 500 ms");
        }

        if (!was_streaming) {
          cam->stop_streaming();
        }
      }
      // Signal waiting HTTP handler
      if (this->snapshot_done_) xSemaphoreGive(this->snapshot_done_);
    }
#endif  // HAVE_CAMERA
  if (!this->capture_requested_ || this->capture_in_progress_)
    return;

  this->capture_in_progress_ = true;
  this->capture_requested_ = false;

  lv_draw_buf_t *img = grab_lvgl_rgb565();
  if (!img) {
    ESP_LOGW(TAG, "Main-loop capture returned NULL image");
    this->capture_in_progress_ = false;
    return;
  }

  uint8_t *png_buf = nullptr;
  size_t png_size = 0;
  bool ok = encode_png_to_buffer(img, &png_buf, &png_size);

  lv_draw_buf_destroy(img);

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
      this->last_capture_epoch_ = static_cast<uint32_t>(::time(nullptr));
      xSemaphoreGive(this->png_mutex_);
    }
  } else {
    if (this->last_png_buf_ != nullptr) {
      my_lvgl_free(this->last_png_buf_);
    }
    this->last_png_buf_ = png_buf;
    this->last_png_size_ = png_size;
    this->last_capture_epoch_ = static_cast<uint32_t>(::time(nullptr));
  }

  this->write_png_to_sd_(png_buf, png_size);

  ESP_LOGD(TAG, "PNG image cached, %u bytes", (unsigned) png_size);
  this->capture_in_progress_ = false;
}

}  // namespace screenshot
}  // namespace esphome

#endif  // USE_ESP32
