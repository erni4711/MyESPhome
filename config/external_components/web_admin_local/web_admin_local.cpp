#include "web_admin_local.h"
#include "tiles_lvgl.h"
#include "ha_ws_client.h"

#include "esphome/components/web_server_idf/web_server_idf.h"
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

LocalHandler::LocalHandler(const std::string& base) : base_(base) {}

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

void WebAdminLocal::setup() {
}

void WebAdminLocal::start() {
  ESP_LOGI(TAG, "API client connected; web admin startup queued");
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
  auto* handler = new LocalHandler(base);
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

  // ── LVGL tile renderer ────────────────────────────────────────────────────
  // Create the global renderer so ApiTilesHandler can call refresh_folder()
  // after a successful tile save.
  set_home_assistant_credentials(this->home_assistant_url_, this->home_assistant_token_);
  if (g_tiles_renderer == nullptr) {
    g_tiles_renderer = new TilesLvglRenderer();
    // Use the screen configured and initialized by ESPHome's LVGL component.
    g_tiles_renderer->setup();
    // The renderer uses ESPHome's single active LVGL screen, so loading every
    // folder at startup would leave the last (usually empty) folder visible.
    // Start explicitly on the home folder instead.
    g_tiles_renderer->show_folder(0);
    ESP_LOGI(TAG, "TilesLvglRenderer created; home page load queued");
  }

  // ── Home Assistant websocket live updates ────────────────────────────────
  // refresh_all()/show_folder() above already populated the entity filter
  // (see TilesLvglRenderer::refresh_folder), so start the client last.
  ha_ws_client_configure(this->home_assistant_url_, this->home_assistant_token_);
  ha_ws_client_start();
}

void WebAdminLocal::loop() {
  if (start_requested_.exchange(false, std::memory_order_acquire)) {
    ESP_LOGI(TAG, "Processing queued web admin startup");
    start_internal();
  }
  if (!started_) {
    return;
  }
  if (g_tiles_renderer != nullptr) {
    g_tiles_renderer->process_pending_refreshes();
  }
  // Drain queued Home Assistant state updates and apply them to LVGL
  // widgets. This is the only place ha websocket updates touch LVGL.
  ha_ws_client_loop();
}

}  // namespace web_admin_local
