#pragma once
#include "esphome.h"
#include "esphome/components/web_server_base/web_server_base.h"

// Forward-declare LVGL component to avoid pulling in all LVGL headers here.
namespace esphome::lvgl { class LvglComponent; }

namespace web_admin_local {

class WebAdminLocal : public esphome::Component {
 public:
  explicit WebAdminLocal(esphome::web_server_base::WebServerBase* server)
    : server_(server), url_prefix_("admin") {}
  void setup() override;
  void loop() override;
  void set_url_prefix(const char* prefix) {
    if (prefix && strlen(prefix) > 0) url_prefix_ = std::string(prefix);
  }
  void set_home_assistant_url(const char* url) {
    home_assistant_url_ = url ? std::string(url) : std::string();
  }
  void set_home_assistant_token(const char* token) {
    home_assistant_token_ = token ? std::string(token) : std::string();
  }
  // Optional: connect the LVGL component so tile changes update the display.
  void set_lvgl(esphome::lvgl::LvglComponent *lv) { lvgl_ = lv; }
  float get_setup_priority() const override {
    // Initialize after web_server_base (which has priority 100)
    return esphome::setup_priority::AFTER_CONNECTION;
  }


 private:
  esphome::web_server_base::WebServerBase* server_;
  std::string url_prefix_;
  std::string home_assistant_url_;
  std::string home_assistant_token_;
  esphome::lvgl::LvglComponent *lvgl_ = nullptr;
};

class LocalHandler : public AsyncWebHandler {
  public:
    LocalHandler(const std::string &base);
    bool canHandle(AsyncWebServerRequest *request) const override;
    void handleRequest(AsyncWebServerRequest *request) override;
    bool isRequestHandlerTrivial() const override;
  protected:
    void handleRoot(AsyncWebServerRequest *request);
    void handleAssetRequest(AsyncWebServerRequest *request);
    void sendWebFontRegular(AsyncWebServerRequest *request);
    void sendWebFontSemibold(AsyncWebServerRequest *request);
    void sendAdminCssAsset(AsyncWebServerRequest *request);
    void sendAdminJsAsset(AsyncWebServerRequest *request);
    std::string getConfigPage();
    std::string getSuccessPage();
  private:
    std::string base_;
};

}
