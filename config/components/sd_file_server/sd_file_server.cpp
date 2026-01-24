#include "sd_file_server.h"
#include "esphome/components/sd_mmc_card/sd_mmc_card.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace sd_file_server {

static const char *TAG = "sd_file_server";

SDFileServer::SDFileServer(web_server::WebServer *base) : base_(base) {}

void SDFileServer::setup() {
  if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->add_handler(this);
  } else {
    ESP_LOGW(TAG, "web_server_base not available; SD file server handler not registered");
  }
}

void SDFileServer::dump_config() {
  ESP_LOGCONFIG(TAG, "SD File Server:");
  ESP_LOGCONFIG(TAG, "  Address: %s", network::get_use_address());
  ESP_LOGCONFIG(TAG, "  Url Prefix: %s", this->url_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Root Path: %s", this->root_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Deletation Enabled: %s", TRUEFALSE(this->deletion_enabled_));
  ESP_LOGCONFIG(TAG, "  Download Enabled : %s", TRUEFALSE(this->download_enabled_));
  ESP_LOGCONFIG(TAG, "  Upload Enabled : %s", TRUEFALSE(this->upload_enabled_));
}

bool SDFileServer::canHandle(AsyncWebServerRequest *request) const {
  ESP_LOGD(TAG, "can handle %s %u", request->url().c_str(),
           str_startswith(std::string(request->url().c_str()), this->build_prefix()));
  return str_startswith(std::string(request->url().c_str()), this->build_prefix());
}

void SDFileServer::handleRequest(AsyncWebServerRequest *request) {
  ESP_LOGD(TAG, "%s", request->url().c_str());
  if (str_startswith(std::string(request->url().c_str()), this->build_prefix())) {
    if (request->method() == HTTP_GET) {
      this->handle_get(request);
      return;
    }
    if (request->method() == HTTP_DELETE) {
      this->handle_delete(request);
      return;
    }
  }
}

void SDFileServer::handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,
                                uint8_t *data, size_t len, bool final) {
  if (!this->upload_enabled_) {
    request->send(401, "application/json", "{ \"error\": \"file upload is disabled\" }");
    return;
  }
  std::string extracted = this->extract_path_from_url(std::string(request->url().c_str()));
  std::string path = this->build_absolute_path(extracted);

  // Build absolute file path
  std::string file_name = filename;
  std::string file_path = Path::join(path, file_name);
  if (index == 0) {
    ESP_LOGD(TAG, "uploading file %s to %s", file_name.c_str(), path.c_str());
    // remove existing file (ignore result) then start first chunk
    this->sd_mmc_card_->delete_file(file_path);
    this->sd_mmc_card_->append_file_chunk(file_path, data, len, true);
    return;
  }
  this->sd_mmc_card_->append_file_chunk(file_path, data, len, false);
  if (final) {
    auto response = request->beginResponse(201, "text/html", "upload success");
    response->addHeader("Connection", "close");
    request->send(response);
    return;
  }
}

void SDFileServer::set_url_prefix(std::string const &prefix) { this->url_prefix_ = prefix; }

void SDFileServer::set_root_path(std::string const &path) { this->root_path_ = path; }

void SDFileServer::set_sd_mmc_card(::esphome::sd_card::SdMmcCard *card) { this->sd_mmc_card_ = card; }

void SDFileServer::set_deletion_enabled(bool allow) { this->deletion_enabled_ = allow; }

void SDFileServer::set_download_enabled(bool allow) { this->download_enabled_ = allow; }

void SDFileServer::set_upload_enabled(bool allow) { this->upload_enabled_ = allow; }

void SDFileServer::handle_get(AsyncWebServerRequest *request) const {
  std::string extracted = this->extract_path_from_url(std::string(request->url().c_str()));
  std::string path = this->build_absolute_path(extracted);

  // Try to read file; if successful, treat as file download, otherwise list directory
  std::string file_contents;
  if (this->sd_mmc_card_->read_file_to_string(path, file_contents)) {
    handle_download(request, path);
    return;
  }

  handle_index(request, path);
}

// Previously this component tried to render an HTML table using a FileInfo
// structure. The current sd SPI shim exposes a JSON directory listing and
// simple read/append/delete helpers. For now, render the raw JSON listing
// from the sd card shim to keep the feature working without depending on
// a FileInfo type.
void SDFileServer::handle_index(AsyncWebServerRequest *request, std::string const &path) const {
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  std::string json = this->sd_mmc_card_->list_dir_json(path);
  response->print(json.c_str());
  request->send(response);
}

// handle_index now implemented above (replaced earlier implementation).

void SDFileServer::handle_download(AsyncWebServerRequest *request, std::string const &path) const {
  if (!this->download_enabled_) {
    request->send(401, "application/json", "{ \"error\": \"file download is disabled\" }");
    return;
  }

  std::string contents;
  if (!this->sd_mmc_card_->read_file_to_string(path, contents)) {
    request->send(401, "application/json", "{ \"error\": \"failed to read file\" }");
    return;
  }

#ifdef USE_ESP_IDF
  auto *response = request->beginResponse(200, Path::mime_type(path).c_str(), reinterpret_cast<const uint8_t *>(contents.data()), contents.size());
#else
  auto *response = request->beginResponseStream(Path::mime_type(path).c_str(), contents.size());
  response->write(contents.data(), contents.size());
#endif

  request->send(response);
}

std::string SDFileServer::build_prefix() const {
  if (this->url_prefix_.length() == 0 || this->url_prefix_.at(0) != '/')
    return "/" + this->url_prefix_;
  return this->url_prefix_;
}

std::string SDFileServer::extract_path_from_url(std::string const &url) const {
  std::string prefix = this->build_prefix();
  return url.substr(prefix.size(), url.size() - prefix.size());
}

std::string SDFileServer::build_absolute_path(std::string relative_path) const {
  if (relative_path.size() == 0)
    return this->root_path_;

  std::string absolute = Path::join(this->root_path_, relative_path);
  return absolute;
}

// Path implementation (file_name, is_absolute, join, etc.)
// Lightweight implementation used by the file-server handlers.

std::string Path::file_name(std::string const &p) {
  if (p.empty()) return std::string();
  auto pos = p.find_last_of(Path::separator);
  if (pos == std::string::npos) return p;
  return p.substr(pos + 1);
}

bool Path::is_absolute(std::string const &p) { return !p.empty() && p.front() == Path::separator; }

bool Path::trailing_slash(std::string const &p) { return !p.empty() && p.back() == Path::separator; }

std::string Path::join(std::string const &a, std::string const &b) {
  if (b.empty()) return a;
  if (is_absolute(b)) return b;
  if (a.empty()) return b;
  std::string out = a;
  if (!trailing_slash(out)) out.push_back(Path::separator);
  out += b;
  return out;
}

std::string Path::remove_root_path(std::string path, std::string const &root) {
  if (root.empty()) return path;
  if (path.rfind(root, 0) == 0) {
    return path.substr(root.size());
  }
  return path;
}

std::vector<std::string> Path::split_path(std::string path) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < path.size()) {
    if (path[start] == Path::separator) { ++start; continue; }
    auto pos = path.find(Path::separator, start);
    if (pos == std::string::npos) { parts.emplace_back(path.substr(start)); break; }
    parts.emplace_back(path.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::string Path::extension(std::string const &p) {
  auto name = file_name(p);
  auto pos = name.find_last_of('.');
  if (pos == std::string::npos) return std::string();
  return name.substr(pos + 1);
}

std::string Path::file_type(std::string const &p) { return trailing_slash(p) ? "directory" : "file"; }

std::string Path::mime_type(std::string const &p) {
  auto ext = extension(p);
  if (ext == "html" || ext == "htm") return "text/html";
  if (ext == "css") return "text/css";
  if (ext == "js") return "application/javascript";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "json") return "application/json";
  if (ext == "txt") return "text/plain";
  return "application/octet-stream";
}

void SDFileServer::handle_delete(AsyncWebServerRequest *request) {
  if (!this->deletion_enabled_) {
    request->send(401, "application/json", "{ \"error\": \"deletion disabled\" }");
    return;
  }
  std::string extracted = this->extract_path_from_url(std::string(request->url().c_str()));
  std::string path = this->build_absolute_path(extracted);
  bool ok = this->sd_mmc_card_->delete_file(path);
  if (!ok) {
    request->send(500, "application/json", "{ \"error\": \"failed to delete\" }");
    return;
  }
  request->send(200, "application/json", "{ \"success\": true }");
}

}  // namespace sd_file_server
}  // namespace esphome