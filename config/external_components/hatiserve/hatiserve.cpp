#include "hatiserve.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
// #include <sys/statvfs.h>
#include <cerrno>
#include <cstring>
#include <vector>

#include "esphome/core/log.h"
#include "esphome/components/spiffs/spiffs.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_spiffs.h"

extern "C" esp_err_t esp_vfs_fat_info(const char *base_path,
                                      uint64_t *out_total_bytes,
                                      uint64_t *out_free_bytes)
    __attribute__((weak));

namespace hatiserve {

static const char *const TAG = "hatiserve";
static constexpr size_t MAX_FILE_SIZE = 512 * 1024;

static std::string format_bytes(uint64_t bytes) {
  constexpr const char *units[] = {"B", "KB", "MB", "GB"};
  size_t unit = 0;
  double value = static_cast<double>(bytes);
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  char text[32];
  if (unit == 0) {
    snprintf(text, sizeof(text), "%llu %s",
             static_cast<unsigned long long>(bytes), units[unit]);
  } else {
    snprintf(text, sizeof(text), "%.1f %s", value, units[unit]);
  }
  return text;
}

void HATiServe::setup() {
  if (this->base_ == nullptr) {
    ESP_LOGW(TAG, "web_server_base is unavailable; handler not registered");
    return;
  }
  this->base_->add_handler(this);
  if (!esphome::spiffs::ensure_mounted()) {
    ESP_LOGW(TAG, "SPIFFS is not mounted; /%s will be unavailable",
             this->spiffs_prefix_.c_str());
  }
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
  if (request == nullptr) return false;
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
  if (!is_sd && !esphome::spiffs::ensure_mounted()) {
    ESP_LOGW(TAG, "SPIFFS mount check failed while handling %s", url.c_str());
    request->send(503, "text/plain", "SPIFFS is not mounted");
    return;
  }
  const bool delete_request =
      request->method() == HTTP_DELETE ||
      (request->method() == HTTP_POST && request->hasParam("delete"));
  if (delete_request) {
    std::string relative = url.size() > selected_prefix.size()
                                ? url.substr(selected_prefix.size())
                                : "";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    if (!safe_relative_path(relative) || relative.empty()) {
      request->send(400, "text/plain", "Invalid delete path");
      return;
    }
    const std::string path = mount_path + "/" + relative;
    if (!remove_tree(path)) {
      request->send(404, "text/plain", "Unable to delete path");
      return;
    }
    ESP_LOGI(TAG, "Deleted %s", path.c_str());
    request->send(200, "text/plain", "Deleted");
    return;
  }
  if (request->method() == HTTP_POST && request->hasParam("mkdir")) {
    std::string relative = url.size() > selected_prefix.size()
                                ? url.substr(selected_prefix.size())
                                : "";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    while (!relative.empty() && relative.back() == '/') relative.pop_back();
    const std::string name = request->getParam("mkdir")->value().c_str();
    if (!safe_relative_path(relative) || !safe_relative_path(name) ||
        name.empty() || name.find('/') != std::string::npos) {
      request->send(400, "text/plain", "Invalid folder name");
      return;
    }
    const std::string path = mount_path + "/" +
        (relative.empty() ? name : relative + "/" + name);
    if (mkdir(path.c_str(), 0777) != 0) {
      ESP_LOGW(TAG, "Unable to create folder %s: %s", path.c_str(), std::strerror(errno));
      request->send(409, "text/plain", "Unable to create folder");
      return;
    }
    ESP_LOGI(TAG, "Created folder %s", path.c_str());
    request->send(201, "text/plain", "Created");
    return;
  }
  if (request->method() == HTTP_POST && request->hasParam("rename")) {
    std::string relative = url.size() > selected_prefix.size()
                                ? url.substr(selected_prefix.size())
                                : "";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    const std::string target = request->getParam("rename")->value().c_str();
    if (!safe_relative_path(relative) || relative.empty() ||
        !safe_relative_path(target) || target.empty()) {
      request->send(400, "text/plain", "Invalid rename path");
      return;
    }
    const std::string source_path = mount_path + "/" + relative;
    const std::string target_path = mount_path + "/" + target;
    if (rename(source_path.c_str(), target_path.c_str()) != 0) {
      ESP_LOGW(TAG, "Rename failed %s -> %s: %s", source_path.c_str(),
               target_path.c_str(), std::strerror(errno));
      request->send(500, "text/plain", "Unable to rename path");
      return;
    }
    ESP_LOGI(TAG, "Renamed %s -> %s", source_path.c_str(), target_path.c_str());
    request->send(200, "text/plain", "Renamed");
    return;
  }
  // Multipart uploads are delivered through handleUpload(). Do not complete
  // the request here before the final upload chunk has been written.
  if (request->method() == HTTP_POST) return;
  if (request->method() != HTTP_GET) {
    request->send(405, "text/plain", "Method not allowed");
    return;
  }
  this->serve_root(request, selected_prefix, mount_path);
}

void HATiServe::handleUpload(AsyncWebServerRequest *request,
                             const std::string &filename, size_t index,
                             uint8_t *data, size_t len, bool final) {
  if (request == nullptr || filename.empty() || !safe_relative_path(filename)) {
    ESP_LOGW(TAG, "Rejected unsafe upload filename");
    if (final && upload_file_ != nullptr) {
      fclose(upload_file_);
      upload_file_ = nullptr;
    }
    return;
  }
  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(buffer).str();
  const std::string sd_prefix = this->prefix(this->sd_prefix_);
  const bool is_sd = url == sd_prefix || url.starts_with(sd_prefix + "/");
  const std::string selected_prefix = is_sd ? sd_prefix : this->prefix(this->spiffs_prefix_);
  const std::string mount_path = is_sd ? "/sdcard" : "/spiffs";
  std::string directory = url.size() > selected_prefix.size()
                              ? url.substr(selected_prefix.size())
                              : "";
  while (!directory.empty() && directory.front() == '/') directory.erase(directory.begin());
  while (!directory.empty() && directory.back() == '/') directory.pop_back();
  if (!safe_relative_path(directory)) {
    ESP_LOGW(TAG, "Rejected unsafe upload directory");
    return;
  }
  const std::string path = mount_path + "/" +
      (directory.empty() ? filename : directory + "/" + filename);
  if (index == 0) {
    if (upload_file_ != nullptr) fclose(upload_file_);
    upload_path_ = path;
    upload_size_ = 0;
    upload_file_ = fopen(path.c_str(), "wb");
    if (upload_file_ == nullptr) {
      ESP_LOGW(TAG, "Unable to open upload destination %s: %s",
               path.c_str(), std::strerror(errno));
      request->send(500, "text/plain", "Unable to open upload destination");
      return;
    }
    ESP_LOGI(TAG, "Upload started: %s", path.c_str());
  }
  if (upload_file_ == nullptr) return;
  if (len > MAX_FILE_SIZE - std::min(upload_size_, MAX_FILE_SIZE)) {
    ESP_LOGW(TAG, "Upload exceeds %u byte limit: %s",
             static_cast<unsigned>(MAX_FILE_SIZE), path.c_str());
    fclose(upload_file_);
    upload_file_ = nullptr;
    remove(path.c_str());
    request->send(413, "text/plain", "File is too large");
    return;
  }
  if (len > 0 && fwrite(data, 1, len, upload_file_) != len) {
    ESP_LOGW(TAG, "Upload write failed: %s", path.c_str());
    if (upload_file_ != nullptr) {
      fclose(upload_file_);
      upload_file_ = nullptr;
    }
    upload_size_ += len;
    return;
  }
  if (final) {
    fclose(upload_file_);
    upload_file_ = nullptr;
    ESP_LOGI(TAG, "Upload complete: %s", path.c_str());
    request->send(200, "text/plain", "Uploaded");
  }
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
  const bool is_mount_root = relative.empty();
  if (!is_mount_root && stat(path.c_str(), &info) != 0) {
    request->send(404, "text/plain", "Filesystem is not mounted or path was not found");
    return;
  }
  if (is_mount_root || S_ISDIR(info.st_mode)) {
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

    size_t folder_count = 0;
    size_t file_count = 0;
    uint64_t entry_bytes = 0;
    for (const auto &name : names) {
      struct stat entry_info {};
      if (stat((path + "/" + name).c_str(), &entry_info) != 0) continue;
      if (S_ISDIR(entry_info.st_mode)) {
        ++folder_count;
      } else if (S_ISREG(entry_info.st_mode)) {
        ++file_count;
        entry_bytes += static_cast<uint64_t>(entry_info.st_size);
      }
    }

    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    if (is_mount_root && mount_path == "/spiffs") {
      const esp_partition_t *partition = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
      size_t total = 0;
      size_t used = 0;
      if (partition != nullptr &&
          esp_spiffs_info(partition->label, &total, &used) == ESP_OK) {
        total_bytes = total;
        used_bytes = used;
      }
    } else if (mount_path == "/sdcard" && esp_vfs_fat_info != nullptr &&
               esp_vfs_fat_info(mount_path.c_str(), &total_bytes,
                                &used_bytes) == ESP_OK) {
      const uint64_t free_bytes = used_bytes;
      used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    } else {
      #if 0
      struct statvfs volume {};
      if (statvfs(mount_path.c_str(), &volume) == 0) {
        total_bytes = static_cast<uint64_t>(volume.f_blocks) * volume.f_frsize;
        const uint64_t free_bytes =
            static_cast<uint64_t>(volume.f_bavail) * volume.f_frsize;
        used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
      }
      #endif
    }

    AsyncResponseStream *response = request->beginResponseStream("text/html; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    const std::string escaped_url = html_escape(url);
    response->print(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>HATiServe</title><style>"
        ":root{color-scheme:dark;--bg:#10151d;--card:#192331;--line:#2b3a4d;"
        "--text:#edf4fb;--muted:#9eb0c3;--accent:#42c7a5;--danger:#e97878}"
        "*{box-sizing:border-box}body{margin:0;padding:28px;background:var(--bg);"
        "color:var(--text);font:15px system-ui,-apple-system,Segoe UI,sans-serif}"
        "main{max-width:980px;margin:auto}h1{margin:0 0 6px;font-size:28px}"
        ".path{color:var(--muted);margin-bottom:22px}.cards{display:grid;"
        "grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-bottom:18px}"
        ".card,.toolbar,.entry{background:var(--card);border:1px solid var(--line);"
        "border-radius:12px}.card{padding:15px}.label{color:var(--muted);font-size:12px;"
        "text-transform:uppercase;letter-spacing:.08em}.value{font-size:21px;margin-top:5px}"
        ".toolbar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;padding:14px;margin-bottom:18px}"
        "input{background:#0d131b;color:var(--text);border:1px solid var(--line);"
        "border-radius:7px;padding:9px}button{border:0;border-radius:7px;padding:9px 12px;"
        "background:var(--accent);color:#07120f;font-weight:600;cursor:pointer}"
        "button.secondary{background:#304257;color:var(--text)}button.danger{background:var(--danger)}"
        ".entry{display:grid;grid-template-columns:42px minmax(0,1fr) 110px 180px;"
        "gap:12px;align-items:center;padding:11px 14px;margin:8px 0}.icon{font-size:23px}"
        ".name{color:var(--text);text-decoration:none;font-weight:600;overflow-wrap:anywhere}"
        ".kind,.size{color:var(--muted);font-size:13px}.actions{display:flex;gap:6px;justify-content:flex-end}"
        ".actions button{padding:6px 8px;font-size:12px}@media(max-width:700px){"
        ".entry{grid-template-columns:34px minmax(0,1fr);}.kind,.size{grid-column:2}.actions{grid-column:2;justify-content:flex-start}}"
        "</style></head><body><main><h1>Filesystem browser</h1><div class=\"path\">");
    response->print(escaped_url.c_str());
    response->print("</div><section class=\"cards\">");
    response->printf("<div class=\"card\"><div class=\"label\">Location</div><div class=\"value\">%s</div></div>"
                     "<div class=\"card\"><div class=\"label\">Contents</div><div class=\"value\">%u folders · %u files</div></div>"
                     "<div class=\"card\"><div class=\"label\">Listed size</div><div class=\"value\">%s</div></div>",
                     mount_path.c_str(), static_cast<unsigned>(folder_count),
                     static_cast<unsigned>(file_count), format_bytes(entry_bytes).c_str());
    if (total_bytes > 0) {
      const unsigned percent = static_cast<unsigned>(
          std::min<uint64_t>(100, (used_bytes * 100) / total_bytes));
      response->printf("<div class=\"card\"><div class=\"label\">Volume usage</div>"
                       "<div class=\"value\">%s / %s (%u%%)</div></div>",
                       format_bytes(used_bytes).c_str(), format_bytes(total_bytes).c_str(),
                       percent);
    }
    response->printf("</section><section class=\"toolbar\">"
                     "<form method=\"post\" enctype=\"multipart/form-data\" action=\"%s\">"
                     "<input type=\"file\" name=\"file\" required><button>Upload file</button></form>"
                     "<form method=\"post\" action=\"%s\">"
                     "<input name=\"mkdir\" placeholder=\"New folder\" required>"
                     "<button class=\"secondary\">Create folder</button></form></section><section>",
                     escaped_url.c_str(), escaped_url.c_str());
    if (!relative.empty()) {
      const auto slash = relative.find_last_of('/');
      const std::string parent = slash == std::string::npos ? "" : relative.substr(0, slash);
      response->printf("<div class=\"entry\"><div class=\"icon\">&lt;-</div><a class=\"name\" href=\"%s%s\">Parent folder</a>"
                       "<div class=\"kind\">Directory</div><div class=\"size\">..</div></div>",
                       url_prefix.c_str(), parent.empty() ? "/" : ("/" + parent).c_str());
    }
    for (const auto &name : names) {
      const std::string href = url + (url.back() == '/' ? "" : "/") + name;
      struct stat entry_info {};
      const bool exists = stat((path + "/" + name).c_str(), &entry_info) == 0;
      const bool is_directory = exists && S_ISDIR(entry_info.st_mode);
      const std::string escaped_href = html_escape(href);
      const std::string escaped_name = html_escape(name);
      const char *icon = is_directory ? "&#128193;" : "&#128196;";
      const char *kind = is_directory ? "Directory" : "File";
      const std::string size = is_directory || !exists
                                   ? "--"
                                   : format_bytes(static_cast<uint64_t>(entry_info.st_size));
      response->printf("<div class=\"entry\"><div class=\"icon\">%s</div>"
                       "<a class=\"name\" href=\"%s\">%s%s</a><div class=\"kind\">%s</div>"
                       "<div class=\"actions\"><span class=\"size\">%s</span>"
                       "<button class=\"secondary\" onclick=\"renameEntry('%s');return false\">Rename</button>"
                       "<button class=\"danger\" onclick=\"removeEntry('%s');return false\">Delete</button>"
                       "</div></div>",
                       icon, escaped_href.c_str(), escaped_name.c_str(),
                       is_directory ? "/" : "", kind, size.c_str(),
                       escaped_href.c_str(), escaped_href.c_str());
    }
    response->print("</section><script>"
                    "async function removeEntry(u){if(confirm('Delete this item?')){"
                    "const r=await fetch(u+'?delete=1',{method:'POST'});"
                    "if(r.ok)location.reload();else alert(await r.text());}}"
                    "async function renameEntry(u){const n=prompt('New path relative to filesystem root:');"
                    "if(n){const r=await fetch(u+'?rename='+encodeURIComponent(n),{method:'POST'});"
                    "if(r.ok)location.reload();else alert(await r.text());}}"
                    "</script>");
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
  const std::string content_type = mime_type(path);
  const size_t file_size = static_cast<size_t>(info.st_size);
  uint8_t *contents = static_cast<uint8_t *>(
      heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (contents == nullptr) {
    fclose(file);
    request->send(507, "text/plain", "Not enough memory to serve file");
    return;
  }
  const size_t read = fread(contents, 1, file_size, file);
  fclose(file);
  if (read != file_size) {
    heap_caps_free(contents);
    request->send(500, "text/plain", "Unable to read file");
    return;
  }
  // web_server_idf sends this response synchronously, so the PSRAM buffer
  // remains valid until the HTTP transmission has completed.
  request->send(request->beginResponse(200, content_type.c_str(), contents,
                                       file_size));
  heap_caps_free(contents);
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

bool HATiServe::remove_tree(const std::string &path) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) return false;
  if (S_ISDIR(info.st_mode)) {
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) return false;
    bool ok = true;
    for (dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      const std::string child = path + "/" + entry->d_name;
      if (!remove_tree(child)) ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
  }
  return unlink(path.c_str()) == 0;
}

}  // namespace hatiserve
