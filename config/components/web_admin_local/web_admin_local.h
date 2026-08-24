#pragma once
#include "esphome.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace web_admin_local {

class WebAdminLocal : public esphome::Component {
 public:
  explicit WebAdminLocal(esphome::web_server_base::WebServerBase* server) : server_(server), url_prefix_("admin") {}
  void setup() override;
  void loop() override {}
  void set_url_prefix(const char* prefix) { if (prefix && strlen(prefix)>0) url_prefix_ = std::string(prefix); }

 private:
  esphome::web_server_base::WebServerBase* server_;
  std::string url_prefix_;
};

}  // namespace web_admin_local
