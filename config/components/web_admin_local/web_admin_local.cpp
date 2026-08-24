#include "web_admin_local.h"
#include "esphome/components/web_server_idf/web_server_idf.h"

namespace web_admin_local {

class LocalHandler : public AsyncWebHandler {
 public:
  LocalHandler(const std::string &base) : base_(base) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    const auto url = request->url();
    return request->method() == HTTP_GET && (url == base_ || url == "/");
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    if (request->url() == "/") {
      request->redirect(base_);
      return;
    }
    const char* body = "<!doctype html><html><head><title>Admin</title></head><body><h1>Admin Page</h1><p>Welcome to the local admin page.</p></body></html>";
    request->send(200, "text/html; charset=utf-8", body);
  }
  bool isRequestHandlerTrivial() const override { return false; }

 private:
  std::string base_;
};

void WebAdminLocal::setup() {
  const std::string base = std::string("/") + url_prefix_;
  auto *handler = new LocalHandler(base);
  this->server_->add_handler(handler);
}

}  // namespace web_admin_local
