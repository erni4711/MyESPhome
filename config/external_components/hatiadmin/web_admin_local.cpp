#include "web_admin_local.h"
#include "../hatilvgl/tiles_lvgl.h"
#include "../hatilvgl/hatilvgl.h"


#include "esphome/components/web_server_idf/web_server_idf.h"
#include "esphome/core/helpers.h"
#include <esp_log.h>

// Forward-declare asset path accessors defined in web_admin_assets.cpp
const char* adminCssAssetPath();
const char* adminJsAssetPath();

static const char* TAG = "web_admin_local";

namespace web_admin_local {

static std::string request_url(AsyncWebServerRequest *request) {
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  return request->url_to(url_buffer).str();
}

LocalHandler::LocalHandler(const std::string& base, WebAdminLocal *owner)
    : base_(base), owner_(owner) {}

static const char* method_to_string(AsyncWebServerRequest* req) {
  switch (req->method()) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PATCH: return "PATCH";
    default: return "OTHER";
  }
}

bool LocalHandler::canHandle(AsyncWebServerRequest* request) const {
  const auto url = request_url(request);
  ESP_LOGD(TAG, "canHandle check: url=%s method=%s base=%s", url.c_str(), method_to_string(request), base_.c_str());
  if (url == base_) return true;
  if (url == base_ + "/save" && request->method() == HTTP_POST) return true;
  if (url.starts_with(base_ + "/assets")) return true;
  return false;
}

void LocalHandler::handleRequest(AsyncWebServerRequest* request) {
  ESP_LOGI(TAG, "handleRequest: %s %s", method_to_string(request), request_url(request).c_str());

  if (request_url(request) == "/") {
    request->redirect(base_);
    return;
  }
  if (request_url(request) == base_) {
    handleRoot(request);
    return;
  }
  if (request_url(request) == base_ + "/save" && request->method() == HTTP_POST) {
    handleSave(request);
    return;
  }
  if (request_url(request).starts_with(base_+ "/assets")) {
    handleAssetRequest(request);
    return;
  }

  request->send(404, "text/plain", "Not Found");
}

void LocalHandler::handleAssetRequest(AsyncWebServerRequest* request) {
  const auto url = request_url(request);
  if (url.ends_with("inter-4.1-regular.woff2")) {
    sendWebFontRegular(request);
  } else if (url.ends_with("inter-4.1-semibold.woff2")) {
    sendWebFontSemibold(request);
  } else if (url == adminCssAssetPath()) {
    sendAdminCssAsset(request);
  } else if (url == adminJsAssetPath()) {
    sendAdminJsAsset(request);
  } else {
    request->send(404, "text/plain", "Not Found");
  }
}

bool LocalHandler::isRequestHandlerTrivial() const { return false; }

void LocalHandler::handleSave(AsyncWebServerRequest *request) {
  if (owner_ == nullptr || !request->hasParam("ha_url")) {
    request->send(400, "text/plain", "Home Assistant URL is required");
    return;
  }
  const std::string url = request->getParam("ha_url")->value().c_str();
  const std::string token =
      request->hasParam("ha_token")
          ? request->getParam("ha_token")->value().c_str()
          : std::string();
  if (url.empty() || (url.rfind("http://", 0) != 0 &&
                      url.rfind("https://", 0) != 0) ||
      url.find_first_of("\r\n") != std::string::npos ||
      token.find_first_of("\r\n") != std::string::npos) {
    request->send(400, "text/plain", "Invalid Home Assistant configuration");
    return;
  }
  if (!owner_->save_home_assistant_credentials(url, token)) {
    request->send(500, "text/plain", "Unable to save Home Assistant configuration");
    return;
  }
  request->send(200, "text/html; charset=utf-8", getSuccessPage().c_str());
}

void WebAdminLocal::setup() {
  this->credentials_pref_ =
      esphome::global_preferences->make_preference<StoredCredentials>(
          esphome::fnv1_hash("hatiadmin_credentials"), true);
  StoredCredentials stored{};
  if (this->credentials_pref_.load(&stored) && stored.url[0] != '\0') {
    this->home_assistant_url_ = stored.url;
    this->home_assistant_token_ = stored.token;
    ESP_LOGI(TAG, "Loaded persisted Home Assistant configuration");
  }
}

bool WebAdminLocal::save_home_assistant_credentials(const std::string &url,
                                                    const std::string &token) {
  const std::string effective_token =
      token.empty() ? this->home_assistant_token_ : token;
  StoredCredentials stored{};
  if (url.empty() || effective_token.empty() || url.size() >= sizeof(stored.url) ||
      effective_token.size() >= sizeof(stored.token)) {
    return false;
  }
  this->home_assistant_url_ = url;
  this->home_assistant_token_ = effective_token;
  std::strncpy(stored.url, url.c_str(), sizeof(stored.url) - 1);
  std::strncpy(stored.token, effective_token.c_str(), sizeof(stored.token) - 1);
  if (!this->credentials_pref_.save(&stored) ||
      !esphome::global_preferences->sync()) {
    return false;
  }
  hatilvgl_update_home_assistant_credentials(this->home_assistant_url_,
                                             this->home_assistant_token_);
  return true;
}

void WebAdminLocal::start() {
  ESP_LOGI(TAG, "API client connected; web admin startup queued");
  hatilvgl_set_api_connected(true);
  start_requested_.store(true, std::memory_order_release);
}

void WebAdminLocal::start_internal() {
  if (started_) {
    ESP_LOGD(TAG, "Web admin startup already completed");
    return;
  }
  started_ = true;
  ESP_LOGI(TAG, "Starting web admin from ESPHome loop");

  const std::string base = std::string("/") + url_prefix_;
  auto* handler = new LocalHandler(base, this);
  this->server_->add_handler(handler);

  // Folders API
  auto* folders_api = new web_admin_local::ApiFolderHandler(base);
  this->server_->add_handler(folders_api);

  // Entity options
  auto* entity_opts = new web_admin_local::EntityOptionsHandler(
      base, this->home_assistant_url_, this->home_assistant_token_);
  this->server_->add_handler(entity_opts);

  // Tiles API (POST saves, GET reads)
  auto* tiles_api = new web_admin_local::ApiTilesHandler(base);
  this->server_->add_handler(tiles_api);

  // Tiles admin page
  auto* tiles_page = new web_admin_local::TilesHandler(base);
  this->server_->add_handler(tiles_page);

  // HATiLvgl owns the renderer and starts it independently of the API client.
  set_home_assistant_credentials(this->home_assistant_url_, this->home_assistant_token_);
}

void WebAdminLocal::loop() {
  if (start_requested_.exchange(false, std::memory_order_acquire)) {
    ESP_LOGI(TAG, "Processing queued web admin startup");
    start_internal();
  }
  if (!started_) {
    return;
  }
}

}  // namespace web_admin_local
