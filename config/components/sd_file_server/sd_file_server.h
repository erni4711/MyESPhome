#pragma once
#include "esphome/core/component.h"
#include "esphome/components/web_server/web_server.h"
#include <utility>
#include <string>
#include <vector>

// Forward-declare sd_card shim type to avoid circular includes
namespace esphome {
namespace sd_card {
class SdMmcCard;
}
}
#include <string>
#include <vector>

namespace esphome {
namespace sd_file_server {

class SDFileServer : public Component, public AsyncWebHandler {
 public:
  SDFileServer(web_server::WebServer *base);
  void setup() override;
  void dump_config() override;
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  // Note: use std::string for filenames to avoid Arduino String dependency
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final);
  bool isRequestHandlerTrivial() const override { return false; }

  void set_url_prefix(std::string const &);
  void set_root_path(std::string const &);
  void set_sd_mmc_card(::esphome::sd_card::SdMmcCard *);
  void set_deletion_enabled(bool);
  void set_download_enabled(bool);
  void set_upload_enabled(bool);

 protected:
  web_server::WebServer *base_;
  ::esphome::sd_card::SdMmcCard *sd_mmc_card_;

  std::string url_prefix_;
  std::string root_path_;
  bool deletion_enabled_{};
  bool download_enabled_{};
  bool upload_enabled_{};

  std::string build_prefix() const;
  std::string extract_path_from_url(std::string const &) const;
  std::string build_absolute_path(std::string) const;
  void handle_index(AsyncWebServerRequest *, std::string const &) const;
  void handle_get(AsyncWebServerRequest *) const;
  void handle_delete(AsyncWebServerRequest *);
  void handle_download(AsyncWebServerRequest *, std::string const &) const;
};

struct Path {
  static constexpr char separator = '/';
  static std::string file_name(std::string const &);
  static bool is_absolute(std::string const &);
  static bool trailing_slash(std::string const &);
  static std::string join(std::string const &, std::string const &);
  static std::string remove_root_path(std::string path, std::string const &root);
  static std::vector<std::string> split_path(std::string path);
  static std::string extension(std::string const &);
  static std::string file_type(std::string const &);
  static std::string mime_type(std::string const &);
};

}  // namespace sd_file_server
}  // namespace esphome
