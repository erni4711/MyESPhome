#include "web_admin_local.h"

namespace web_admin_local {

void WebAdminLocal::setup() {
  // Attempt to use the concrete WebServer if available
  auto* concrete = dynamic_cast<esphome::web_server::WebServer*>(this->server_);
  if (!concrete) return;

  const std::string base = std::string("/") + url_prefix_;

  // Root and /admin
  concrete->on(base.c_str(), esphome::web_server::HTTP_GET, [concrete, this]() {
    const char* body = "<!doctype html><html><head><title>Admin</title></head><body><h1>Admin Page</h1><p>Welcome to the local admin page.</p></body></html>";
    concrete->send(200, "text/html; charset=utf-8", body);
  });

  concrete->on("/", esphome::web_server::HTTP_GET, [concrete, this]() {
    const char* body = "<!doctype html><html><head><meta http-equiv=\"refresh\" content=\"0; url=/admin\"></head><body></body></html>";
    concrete->send(302, "text/html; charset=utf-8", body);
  });
}

}  // namespace web_admin_local
