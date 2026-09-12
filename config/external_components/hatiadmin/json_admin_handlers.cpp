#include "json_admin_handlers.h"

#include <ArduinoJson.h>
#include <cstring>
#include <esp_log.h>

namespace web_admin_local {

namespace {
constexpr char TAG[] = "hatiadmin.json";
static std::string request_url(AsyncWebServerRequest *request) {
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  return request->url_to(url_buffer).str();
}
}  // namespace

JsonAdminHandler::JsonAdminHandler(const std::string &path,
                                   uint32_t preference_key,
                                   const char *default_json)
    : path_(path), preference_key_(preference_key), default_json_(default_json) {
  preference_ = esphome::global_preferences->make_preference<StoredJson>(
      preference_key_, true);
}

bool JsonAdminHandler::canHandle(AsyncWebServerRequest *request) const {
  const auto url = request_url(request);
  return url == path_ &&
         (request->method() == HTTP_GET || request->method() == HTTP_POST);
}

void JsonAdminHandler::handleBody(AsyncWebServerRequest *, uint8_t *data,
                                  size_t len, size_t index, size_t total) {
  if (index == 0) {
    body_.clear();
    body_too_large_ = total > kMaxBodySize;
    if (!body_too_large_) body_.reserve(total);
  }
  if (body_too_large_) return;
  if (len > kMaxBodySize - body_.size()) {
    body_too_large_ = true;
    body_.clear();
    return;
  }
  body_.append(reinterpret_cast<const char *>(data), len);
}

void JsonAdminHandler::send_json(AsyncWebServerRequest *request, int status,
                                 const std::string &json) const {
  request->send(status, "application/json; charset=utf-8", json.c_str());
}

std::string JsonAdminHandler::load_json() {
  StoredJson stored{};
  if (preference_.load(&stored) && stored.version == 1 &&
      stored.json[0] != '\0' &&
      strnlen(stored.json, sizeof(stored.json)) < sizeof(stored.json)) {
    return stored.json;
  }
  return default_json_;
}

bool JsonAdminHandler::save_json(const std::string &json) {
  if (json.empty() || json.size() >= kStoredJsonSize) return false;
  StoredJson stored{};
  stored.version = 1;
  memcpy(stored.json, json.data(), json.size());
  stored.json[json.size()] = '\0';
  return preference_.save(&stored) && esphome::global_preferences->sync();
}

void JsonAdminHandler::handleRequest(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_GET) {
    send_json(request, 200, load_json());
    return;
  }

  if (body_too_large_) {
    send_json(request, 413, "{\"success\":false,\"error\":\"Request too large\"}");
    return;
  }

  JsonDocument document;
  const auto error = deserializeJson(document, body_);
  if (error || !document.is<JsonObject>()) {
    ESP_LOGW(TAG, "Invalid JSON received for %s: %s", path_.c_str(),
             error ? error.c_str() : "root is not an object");
    send_json(request, 400, "{\"success\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  if (path_ == "/admin/screensaver") {
    document["success"] = true;
    if (!document["version"].is<int>()) document["version"] = 2;
  } else {
    JsonArray channels = document["channels"].as<JsonArray>();
    if (channels.isNull()) {
      send_json(request, 400,
                "{\"success\":false,\"error\":\"channels must be an array\"}");
      return;
    }
    document["success"] = true;
  }

  std::string normalized;
  serializeJson(document, normalized);
  if (!save_json(normalized)) {
    ESP_LOGE(TAG, "Unable to persist %s", path_.c_str());
    send_json(request, 500,
              "{\"success\":false,\"error\":\"Unable to save configuration\"}");
    return;
  }
  send_json(request, 200, normalized);
}

}  // namespace web_admin_local
