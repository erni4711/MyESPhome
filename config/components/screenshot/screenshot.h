#pragma once

#include "esphome.h"
#include "lvgl.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "freertos/semphr.h"
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

  // Call to register the component at runtime (safe after App initialized)
  static void register_component_runtime();

 protected:
  // Async handler registered with web_server_base
  class Handler : public AsyncWebHandler {
   public:
    explicit Handler(ScreenshotComponent *parent) : parent_(parent) {}
    bool canHandle(AsyncWebServerRequest *request) const override {
      return request->url() == "/screenshot.png";
    }
    void handleRequest(AsyncWebServerRequest *request) override;
    void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
                      size_t len, bool final) override {}
    void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {}
    bool isRequestHandlerTrivial() const override { return false; }

   protected:
    ScreenshotComponent *parent_;
  };

  Handler *handler_{nullptr};
  // Pending request pointer held until processed in `loop()` (main thread)
  web_server_idf::AsyncWebServerRequest *pending_request_{nullptr};
  bool processing_{false};
  // Semaphore signaled by main-loop when capture is complete
  SemaphoreHandle_t capture_done_{nullptr};
  // Resulting snapshot allocated by LVGL; set in main-loop and consumed by handler
  lv_img_dsc_t *capture_result_{nullptr};
  ::esphome::sd_card::SdMmcCard *sd_mmc_card_{nullptr};
  ::esphome::sd_card::SdSpiCard *sd_spi_card_{nullptr};

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

  bool write_png_to_sd_(const uint8_t *png_buf, size_t png_size);
};

}  // namespace screenshot
}  // namespace esphome

// Provide an alternative name expected by generated code
namespace esphome {
namespace screenshot {
using Screenshot = ScreenshotComponent;
}  // namespace screenshot
}  // namespace esphome
