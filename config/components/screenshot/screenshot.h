#pragma once

#include "esphome.h"
#include "lvgl.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "../p4_camera/p4_camera.h"
// Forward-declare P4Camera for build robustness in case include ordering differs
namespace esphome { namespace p4_camera { class P4Camera; } }
#include "freertos/semphr.h"
#include <array>
#include <string>
#include <vector>

// Some server APIs use `String` in signatures; alias to std::string for ESP-IDF builds
using String = std::string;

namespace esphome {
namespace sd_card {
class SdSpiCard;
}  // namespace sd_card
namespace sd_card {
class SdMmcCard;
}  // namespace sd_card
namespace screenshot {

class ScreenshotComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_sd_mmc_card(::esphome::sd_card::SdMmcCard *card) { this->sd_mmc_card_ = card; }
  void set_sd_spi_card(::esphome::sd_card::SdSpiCard *card) { this->sd_spi_card_ = card; }
  void set_camera(::esphome::p4_camera::P4Camera *camera) { this->camera_ = camera; }

  // Call to register the component at runtime (safe after App initialized)
  static void register_component_runtime();

 protected:
  // Async handler registered with web_server_base
  class Handler : public AsyncWebHandler {
   public:
    explicit Handler(ScreenshotComponent *parent) : parent_(parent) {}
    bool canHandle(AsyncWebServerRequest *request) const override {
      if (request == nullptr) return false;
      char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
      return request->url_to(url_buf) == "/screenshot.png";
    }
    void handleRequest(AsyncWebServerRequest *request) override;
    void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                      size_t len, bool final) override {}
    void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {}
    bool isRequestHandlerTrivial() const override { return false; }

   protected:
    ScreenshotComponent *parent_;
  };

    class CaptureHandler : public AsyncWebHandler {
     public:
      explicit CaptureHandler(ScreenshotComponent *parent) : parent_(parent) {}
      bool canHandle(AsyncWebServerRequest *request) const override {
        if (request == nullptr) return false;
        char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
        return request->url_to(url_buf) == "/capture";
      }
      void handleRequest(AsyncWebServerRequest *request) override;
      void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                        size_t len, bool final) override {}
      void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {}
      bool isRequestHandlerTrivial() const override { return false; }

     protected:
      ScreenshotComponent *parent_;
    };

    class SnapshotHandler : public AsyncWebHandler {
     public:
      explicit SnapshotHandler(ScreenshotComponent *parent) : parent_(parent) {}
      bool canHandle(AsyncWebServerRequest *request) const override {
        if (request == nullptr) return false;
        char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
        return request->url_to(url_buf) == "/snapshot.jpg";
      }
      void handleRequest(AsyncWebServerRequest *request) override;
      void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                        size_t len, bool final) override {}
      void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                      size_t total) override {}
      bool isRequestHandlerTrivial() const override { return false; }

     protected:
      ScreenshotComponent *parent_;
    };

    // MJPEG video stream — multipart/x-mixed-replace
    class VideoHandler : public AsyncWebHandler {
     public:
      explicit VideoHandler(ScreenshotComponent *parent) : parent_(parent) {}
      bool canHandle(AsyncWebServerRequest *request) const override {
        if (request == nullptr) return false;
        char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
        return request->url_to(url_buf) == "/video";
      }
      void handleRequest(AsyncWebServerRequest *request) override;
      void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                        size_t len, bool final) override {}
      void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                      size_t total) override {}
      bool isRequestHandlerTrivial() const override { return false; }

     protected:
      ScreenshotComponent *parent_;
    };

  Handler *handler_{nullptr};
  CaptureHandler *capture_handler_{nullptr};
  SnapshotHandler *snapshot_handler_{nullptr};
  VideoHandler *video_handler_{nullptr};
  // Pending request pointer held until processed in `loop()` (main thread)
  web_server_idf::AsyncWebServerRequest *pending_request_{nullptr};
  bool processing_{false};
  // Semaphore signaled by main-loop when capture is complete
  SemaphoreHandle_t capture_done_{nullptr};
  // Resulting snapshot allocated by LVGL; set in main-loop and consumed by handler
  lv_img_dsc_t *capture_result_{nullptr};
  ::esphome::sd_card::SdMmcCard *sd_mmc_card_{nullptr};
  ::esphome::sd_card::SdSpiCard *sd_spi_card_{nullptr};
#if HAVE_CAMERA  
  ::esphome::p4_camera::P4Camera *camera_{nullptr};
#endif  

  // Cached PNG image data for quick HTTP responses
  uint8_t *last_png_buf_{nullptr};
  size_t last_png_size_{0};
  bool capture_requested_{false};
  bool capture_in_progress_{false};
  SemaphoreHandle_t png_mutex_{nullptr};
  bool save_requested_{false};
  bool last_save_ok_{false};
  std::string last_save_path_{};
  uint32_t last_save_epoch_{0};
  uint32_t last_capture_epoch_{0};

  bool camera_capture_requested_{false};
  bool camera_capture_in_progress_{false};
  bool camera_waiting_for_frame_{false};
  bool camera_started_streaming_{false};
  uint32_t camera_capture_start_ms_{0};
  bool last_camera_save_ok_{false};
  std::string last_camera_save_path_{};
  uint32_t last_camera_save_epoch_{0};
  uint32_t last_camera_capture_epoch_{0};
  SemaphoreHandle_t camera_mutex_{nullptr};

  // ---- /snapshot.jpg state ----
  volatile bool snapshot_requested_{false};
  volatile bool snapshot_in_progress_{false};
  SemaphoreHandle_t snapshot_done_{nullptr};
  uint8_t *snapshot_jpeg_buf_{nullptr};
  size_t snapshot_jpeg_size_{0};

  // ---- /video MJPEG stream state ----
  volatile bool video_streaming_{false};  // true while a client is connected

  bool write_png_to_sd_(const uint8_t *png_buf, size_t png_size);
  bool write_camera_png_to_sd_(const uint8_t *rgb565_buf, uint16_t width, uint16_t height);
  bool write_camera_jpeg_to_sd_(const uint8_t *jpeg_buf, size_t jpeg_size);
  bool write_snapshot_jpeg_to_sd_(const uint8_t *jpeg_buf, size_t jpeg_size);
};

}  // namespace screenshot
}  // namespace esphome

// Provide an alternative name expected by generated code
namespace esphome {
namespace screenshot {
using Screenshot = ScreenshotComponent;
}  // namespace screenshot
}  // namespace esphome
