#pragma once
#include "web_admin_local.h"

namespace web_admin_local {

class TilesHandler : public AsyncWebHandler {
 public:
  explicit TilesHandler(const std::string &base);
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }
 private:
  std::string base_;
};

// API handler for /api/tiles and <base>/tiles
// GET ?folder=N returns JSON tile array; POST saves a new config.
class ApiTilesHandler : public AsyncWebHandler {
 public:
  explicit ApiTilesHandler(const std::string &base);
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  // Called by the framework for each chunk of a raw-body POST (e.g. application/json).
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }
 private:
  std::string base_;
  std::string body_buf_;
  bool body_too_large_ = false;
};

// Handles /api/folders (GET) and /api/folders/tab (GET)
// so the admin SPA can enumerate folders and dynamically load tab HTML.
class FoldersApiHandler : public AsyncWebHandler {
 public:
  explicit FoldersApiHandler(const std::string &base);
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
 private:
  std::string base_;
  // Build a nav-button + tab-content HTML pair for one folder, suitable for
  // returning in the /api/folders/tab response.
  static std::string buildFolderTabJson(int folder_id, const std::string &name);
};

// Serves /admin/entity_options — returns entity IDs gathered from all stored
// tile grids so the admin.js entity-select dropdowns can show current values.
class EntityOptionsHandler : public AsyncWebHandler {
 public:
  EntityOptionsHandler(const std::string &base, const std::string &home_assistant_url,
                       const std::string &home_assistant_token);
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
 private:
  std::string base_;
 std::string home_assistant_url_;
 std::string home_assistant_token_;
};

} // namespace web_admin_local
