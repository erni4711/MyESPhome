#pragma once

#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include <string>

namespace hatiserve {

class HATiServe : public esphome::Component, public AsyncWebHandler {
 public:
  explicit HATiServe(esphome::web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }

  void set_sd_prefix(const std::string &prefix) { sd_prefix_ = prefix; }
  void set_spiffs_prefix(const std::string &prefix) { spiffs_prefix_ = prefix; }

 protected:
  esphome::web_server_base::WebServerBase *base_;
  std::string sd_prefix_{"sdcard"};
  std::string spiffs_prefix_{"spiffs"};

  std::string prefix(const std::string &root) const;
  void serve_root(AsyncWebServerRequest *request, const std::string &url_prefix,
                  const std::string &mount_path) const;
  static std::string html_escape(const std::string &value);
  static std::string mime_type(const std::string &path);
  static bool safe_relative_path(const std::string &path);
};

}  // namespace hatiserve
