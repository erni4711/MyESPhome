#pragma once

#include "web_admin_local.h"

namespace web_admin_local {

class JsonAdminHandler : public AsyncWebHandler {
 public:
  JsonAdminHandler(const std::string &path, uint32_t preference_key,
                   const char *default_json);
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                  size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 private:
  static constexpr size_t kMaxBodySize = 32 * 1024;
  static constexpr size_t kStoredJsonSize = kMaxBodySize + 1;

  struct StoredJson {
    uint32_t version;
    char json[kStoredJsonSize];
  };

  std::string path_;
  uint32_t preference_key_;
  const char *default_json_;
  std::string body_;
  bool body_too_large_{false};
  esphome::ESPPreferenceObject preference_;

  bool save_json(const std::string &json);
  std::string load_json();
  void send_json(AsyncWebServerRequest *request, int status,
                 const std::string &json) const;
};

}  // namespace web_admin_local
