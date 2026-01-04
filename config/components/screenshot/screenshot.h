#pragma once

#include "esphome.h"
#include "lvgl.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "freertos/semphr.h"
#include <string>

// Some server APIs use `String` in signatures; alias to std::string for ESP-IDF builds
using String = std::string;

namespace esphome {
namespace screenshot {

class ScreenshotComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

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
};

}  // namespace screenshot
}  // namespace esphome

// Provide an alternative name expected by generated code
namespace esphome {
namespace screenshot {
using Screenshot = ScreenshotComponent;
}  // namespace screenshot
}  // namespace esphome
