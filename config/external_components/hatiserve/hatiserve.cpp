#include "hatiserve.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

#include "esphome/core/log.h"
#include "esp_spiffs.h"

namespace hatiserve {

static const char *const TAG = "hatiserve";
static constexpr size_t MAX_FILE_SIZE = 512 * 1024;

void HATiServe::setup() {
  if (this->base_ == nullptr) {
    ESP_LOGW(TAG, "web_server_base is unavailable; handler not registered");
    return;
  }
  this->base_->add_handler(this);
  ESP_LOGI(TAG, "Filesystem server registered at /%s and /%s", this->sd_prefix_.c_str(),
           this->spiffs_prefix_.c_str());
}

void HATiServe::dump_config() {
  ESP_LOGCONFIG(TAG, "HATiServe:");
  ESP_LOGCONFIG(TAG, "  SD URL: /%s -> /sdcard", this->sd_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  SPIFFS URL: /%s -> /spiffs", this->spiffs_prefix_.c_str());
}

float HATiServe::get_setup_priority() const { return esphome::setup_priority::AFTER_WIFI; }

std::string HATiServe::prefix(const std::string &root) const { return "/" + root; }

bool HATiServe::canHandle(AsyncWebServerRequest *request) const {
  if (request == nullptr || request->method() != HTTP_GET) return false;
  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(buffer).str();
  return url == this->prefix(this->sd_prefix_) || url.starts_with(this->prefix(this->sd_prefix_) + "/") ||
         url == this->prefix(this->spiffs_prefix_) ||
         url.starts_with(this->prefix(this->spiffs_prefix_) + "/");
}

void HATiServe::handleRequest(AsyncWebServerRequest *request) {
  if (request == nullptr) return;

  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(buffer).str();
  const std::string sd_prefix = this->prefix(this->sd_prefix_);
  const bool is_sd = url == sd_prefix || url.starts_with(sd_prefix + "/");
  const std::string selected_prefix = is_sd ? sd_prefix : this->prefix(this->spiffs_prefix_);
  const std::string mount_path = is_sd ? "/sdcard" : "/spiffs";
  this->serve_root(request, selected_prefix, mount_path);
}

void HATiServe::serve_root(AsyncWebServerRequest *request, const std::string &url_prefix,
                           const std::string &mount_path) const {
  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(buffer).str();
  std::string relative = url.size() > url_prefix.size() ? url.substr(url_prefix.size()) : "";
  while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
  if (!safe_relative_path(relative)) {
    request->send(400, "text/plain", "Invalid path");
    return;
  }

  const std::string path = relative.empty() ? mount_path : mount_path + "/" + relative;
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    request->send(404, "text/plain", "Filesystem is not mounted or path was not found");
    return;
  }
  if (S_ISDIR(info.st_mode)) {
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) {
      request->send(403, "text/plain", "Unable to open directory");
      return;
    }
    std::vector<std::string> names;
    for (dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
      if (entry->d_name[0] == '.' || names.size() >= 512) continue;
      names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());

    AsyncResponseStream *response = request->beginResponseStream("text/html; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    response->print("<!doctype html><meta charset=\"utf-8\"><title>HATiServe</title><h1>");
    response->print(html_escape(url).c_str());
    response->print("</h1><ul>");
    if (!relative.empty()) {
      const auto slash = relative.find_last_of('/');
      const std::string parent = slash == std::string::npos ? "" : relative.substr(0, slash);
      response->printf("<li><a href=\"%s%s\">..</a></li>", url_prefix.c_str(),
                       parent.empty() ? "/" : ("/" + parent).c_str());
    }
    for (const auto &name : names) {
      const std::string href = url + (url.back() == '/' ? "" : "/") + name;
      response->printf("<li><a href=\"%s\">%s</a></li>", href.c_str(), html_escape(name).c_str());
    }
    response->print("</ul>");
    request->send(response);
    return;
  }

  if (!S_ISREG(info.st_mode) || info.st_size > static_cast<off_t>(MAX_FILE_SIZE)) {
    request->send(413, "text/plain", "File is too large or not a regular file");
    return;
  }
  FILE *file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    request->send(404, "text/plain", "Unable to open file");
    return;
  }
  std::string contents(static_cast<size_t>(info.st_size), '\0');
  const size_t read = fread(contents.data(), 1, contents.size(), file);
  fclose(file);
  if (read != contents.size()) {
    request->send(500, "text/plain", "Unable to read file");
    return;
  }
  request->send(request->beginResponse(200, mime_type(path).c_str(), contents));
}

std::string HATiServe::html_escape(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      default: result += ch; break;
    }
  }
  return result;
}

std::string HATiServe::mime_type(const std::string &path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const std::string ext = path.substr(dot + 1);
  if (ext == "html" || ext == "htm") return "text/html";
  if (ext == "css") return "text/css";
  if (ext == "js") return "application/javascript";
  if (ext == "json") return "application/json";
  if (ext == "txt" || ext == "log") return "text/plain";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  return "application/octet-stream";
}

bool HATiServe::safe_relative_path(const std::string &path) {
  if (path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return false;
  size_t start = 0;
  while (start < path.size()) {
    const size_t end = path.find('/', start);
    const std::string part = path.substr(start, end == std::string::npos ? end : end - start);
    if (part == "..") return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

}  // namespace hatiserve
