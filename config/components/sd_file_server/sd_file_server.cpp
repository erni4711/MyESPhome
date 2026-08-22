#include "sd_file_server.h"
#include "esphome/components/sd_mmc_card/sd_mmc_card.h"
#if defined(__has_include)
#  if __has_include("esphome/components/sd_spi_card/sd_spi_card.h")
#    include "esphome/components/sd_spi_card/sd_spi_card.h"
#    define HAVE_SD_SPI_CARD 1
#  endif
#endif
#ifndef HAVE_SD_SPI_CARD
#  define HAVE_SD_SPI_CARD 0
#endif
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/application.h"
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cctype>
#include <memory>

namespace esphome {
namespace sd_file_server {

static const char *TAG = "sd_file_server";

struct DirEntry {
  std::string name;
  size_t size{};
  bool is_dir{};
};

static std::string url_encode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  auto hex = [](uint8_t v) -> char { return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('A' + (v - 10)); };
  for (unsigned char c : in) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || c == '/') {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out += "%20";
    } else {
      out.push_back('%');
      out.push_back(hex((c >> 4) & 0x0F));
      out.push_back(hex(c & 0x0F));
    }
  }
  return out;
}

static std::string url_decode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '%' && i + 2 < in.size()) {
      int hi = hex_val(in[i + 1]);
      int lo = hex_val(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (c == '+') {
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
  }
  return out;
}

static std::string html_escape(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

static std::string query_param_value(const std::string &query, const std::string &key) {
  std::string needle = key + "=";
  size_t pos = 0;
  while (pos < query.size()) {
    size_t next = query.find('&', pos);
    if (next == std::string::npos) next = query.size();
    std::string part = query.substr(pos, next - pos);
    if (part.rfind(needle, 0) == 0) {
      return url_decode(part.substr(needle.size()));
    }
    pos = next + 1;
  }
  return std::string();
}

static std::string get_query_string(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0) return std::string();
  std::string query;
  query.resize(qlen + 1);
  if (httpd_req_get_url_query_str(req, &query[0], query.size()) == ESP_OK) {
    if (!query.empty() && query.back() == '\0') query.pop_back();
    return query;
  }
  return std::string();
}

static std::string safe_request_url_string(AsyncWebServerRequest *request, std::span<char, 513> buffer) {
  if (request == nullptr) {
    return std::string();
  }
  return request->url_to(buffer).str();
}

static std::vector<DirEntry> parse_list_json(const std::string &json) {
  std::vector<DirEntry> out;
  size_t pos = 0;
  while (true) {
    size_t name_pos = json.find("\"name\"", pos);
    if (name_pos == std::string::npos) break;
    size_t colon = json.find(':', name_pos);
    size_t first_quote = json.find('"', colon + 1);
    size_t second_quote = json.find('"', first_quote + 1);
    if (first_quote == std::string::npos || second_quote == std::string::npos) break;
    DirEntry entry;
    entry.name = json.substr(first_quote + 1, second_quote - first_quote - 1);

    size_t size_pos = json.find("\"size\"", second_quote);
    if (size_pos != std::string::npos) {
      size_t size_colon = json.find(':', size_pos);
      size_t size_end = json.find_first_of(",}", size_colon + 1);
      if (size_colon != std::string::npos && size_end != std::string::npos) {
        entry.size = static_cast<size_t>(std::strtoull(json.substr(size_colon + 1, size_end - size_colon - 1).c_str(), nullptr, 10));
      }
    }

    size_t dir_pos = json.find("\"is_dir\"", second_quote);
    if (dir_pos != std::string::npos) {
      size_t dir_colon = json.find(':', dir_pos);
      size_t dir_end = json.find_first_of(",}", dir_colon + 1);
      if (dir_colon != std::string::npos && dir_end != std::string::npos) {
        std::string val = json.substr(dir_colon + 1, dir_end - dir_colon - 1);
        entry.is_dir = val.find("true") != std::string::npos;
      }
    }

    out.push_back(entry);
    pos = second_quote + 1;
  }
  return out;
}

static const char *status_for_code(int code) {
  switch (code) {
    case 200:
      return HTTPD_200;
    case 400:
      return HTTPD_400;
    case 401:
      return "401 Unauthorized";
    case 411:
      return "411 Length Required";
    case 415:
      return "415 Unsupported Media Type";
    case 503:
      return "503 Service Unavailable";
    case 500:
      return HTTPD_500;
    default:
      return HTTPD_500;
  }
}

static void send_plain_response(httpd_req_t *req, int code, const char *message) {
  if (req == nullptr) return;
  httpd_resp_set_status(req, status_for_code(code));
  if (message != nullptr && *message != '\0') {
    httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send(req, nullptr, 0);
  }
}

struct UploadTaskContext {
  SDFileServer *server;
  httpd_req_t *req;
  std::string path;
  bool append_mode;
};

struct DownloadTaskContext {
  const SDFileServer *server;
  httpd_req_t *req;
  std::string path;
};

void SDFileServer::upload_task(void *param) {
  auto *ctx = static_cast<UploadTaskContext *>(param);
  if (ctx == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  static constexpr size_t UPLOAD_CHUNK_SIZE = 1460;
  static constexpr size_t YIELD_INTERVAL_BYTES = 16 * 1024;

  auto buffer = std::unique_ptr<uint8_t, void (*)(void *)>(
      static_cast<uint8_t *>(heap_caps_malloc(UPLOAD_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      heap_caps_free);
  if (buffer == nullptr) {
    ESP_LOGW(TAG, "upload buffer alloc failed (%u bytes)", (unsigned) UPLOAD_CHUNK_SIZE);
    send_plain_response(ctx->req, 500, "{ \"error\": \"upload buffer alloc failed\" }");
    httpd_req_async_handler_complete(ctx->req);
    ctx->server->upload_in_progress_ = false;
    delete ctx;
    vTaskDelete(nullptr);
    return;
  }

  size_t remaining = ctx->req->content_len;
  bool first = true;
  size_t bytes_since_yield = 0;

  while (remaining > 0) {
    size_t to_read = remaining > UPLOAD_CHUNK_SIZE ? UPLOAD_CHUNK_SIZE : remaining;
    int recv_len = httpd_req_recv(ctx->req, reinterpret_cast<char *>(buffer.get()), to_read);
    if (recv_len <= 0) {
      if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
        vTaskDelay(1);
        continue;
      }
      ESP_LOGW(TAG, "upload recv failed: %d", recv_len);
      httpd_req_async_handler_complete(ctx->req);
      ctx->server->upload_in_progress_ = false;
      delete ctx;
      vTaskDelete(nullptr);
      return;
    }
#if HAVE_SD_SPI_CARD
    if (ctx->server->sd_spi_card_ != nullptr) {
      ctx->server->sd_spi_card_->append_file_chunk(ctx->path, buffer.get(), (size_t) recv_len, first || ctx->append_mode);
    } else
#endif
    if (ctx->server->sd_mmc_card_ != nullptr) {
      if (!ctx->server->sd_mmc_card_->append_file_chunk(ctx->path, buffer.get(), (size_t) recv_len,
                                                        first || ctx->append_mode)) {
        ESP_LOGW(TAG, "upload write failed: %s", ctx->path.c_str());
        httpd_req_async_handler_complete(ctx->req);
        ctx->server->upload_in_progress_ = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
      }
    }

    first = false;
    remaining -= (size_t) recv_len;
    bytes_since_yield += (size_t) recv_len;
    if (bytes_since_yield >= YIELD_INTERVAL_BYTES) {
      vTaskDelay(1);
      bytes_since_yield = 0;
    }
  }

  ESP_LOGI(TAG, "upload complete: %s", ctx->path.c_str());
  send_plain_response(ctx->req, 200, "upload success");
  httpd_req_async_handler_complete(ctx->req);
  ctx->server->upload_in_progress_ = false;
  delete ctx;
  vTaskDelete(nullptr);
}

void SDFileServer::download_task(void *param) {
  auto *ctx = static_cast<DownloadTaskContext *>(param);
  if (ctx == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  auto send_chunk = [req = ctx->req](const uint8_t *data, size_t len) -> bool {
    if (len == 0) return true;
    return httpd_resp_send_chunk(req, reinterpret_cast<const char *>(data), (ssize_t) len) == ESP_OK;
  };

  bool ok = false;
#if HAVE_SD_SPI_CARD
  if (ctx->server->sd_spi_card_ != nullptr) {
    ok = ctx->server->sd_spi_card_->stream_file(ctx->path, send_chunk, 1024);
  } else
#endif
  if (ctx->server->sd_mmc_card_ != nullptr) {
    ok = ctx->server->sd_mmc_card_->stream_file(ctx->path, send_chunk, 1024);
  }

  httpd_resp_send_chunk(ctx->req, nullptr, 0);
  if (!ok) {
    ESP_LOGW(TAG, "stream_file failed for %s", ctx->path.c_str());
  } else {
    ESP_LOGI(TAG, "download complete: %s", ctx->path.c_str());
  }

  httpd_req_async_handler_complete(ctx->req);
  delete ctx;
  vTaskDelete(nullptr);
}

SDFileServer::SDFileServer(web_server_base::WebServerBase *base) : base_(base) {}

void SDFileServer::setup() {
  ESP_LOGI(TAG, "setup() called - initializing SD file server");
  if (this->base_ == nullptr) {
    ESP_LOGW(TAG, "web_server_base not available; SD file server handler not registered");
    return;
  }
  this->base_->add_handler(this);
  ESP_LOGI(TAG, "SD file server handler registered successfully");
}

void SDFileServer::dump_config() {
  std::array<char, network::USE_ADDRESS_BUFFER_SIZE> addr_buf{};
  const char *use_address = network::get_use_address_to(addr_buf);
  if (use_address == nullptr) {
    use_address = "";
  }
  ESP_LOGCONFIG(TAG, "SD File Server Configuration:");
  ESP_LOGCONFIG(TAG, "  Address: %s", use_address);
  ESP_LOGCONFIG(TAG, "  Url Prefix: %s", this->url_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Root Path: %s", this->root_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Deletion Enabled: %s", TRUEFALSE(this->deletion_enabled_));
  ESP_LOGCONFIG(TAG, "  Download Enabled: %s", TRUEFALSE(this->download_enabled_));
  ESP_LOGCONFIG(TAG, "  Upload Enabled: %s", TRUEFALSE(this->upload_enabled_));
}

float SDFileServer::get_setup_priority() const {
  // Initialize after web_server_base (which has priority 100)
  return esphome::setup_priority::AFTER_WIFI;
}

bool SDFileServer::canHandle(AsyncWebServerRequest *request) const {
  if (request == nullptr) return false;

  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(buffer).str();
  const std::string prefix = this->build_prefix();

  if (prefix.empty()) return false;
  if (url.size() < prefix.size()) return false;
  return std::memcmp(url.data(), prefix.data(), prefix.size()) == 0;
}

void SDFileServer::handleRequest(AsyncWebServerRequest *request) {
  if (request == nullptr) return;

  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(buffer).str();
  const std::string prefix = this->build_prefix();

  if (url.size() >= prefix.size() && std::memcmp(url.data(), prefix.data(), prefix.size()) == 0) {
    if (this->upload_in_progress_ && request->method() != HTTP_PUT) {
      httpd_req_t *req = *request;
      ESP_LOGW(TAG, "request blocked during upload: %s", url.c_str());
      send_plain_response(req, 503, "{ \"error\": \"upload in progress\" }");
      return;
    }
    if (request->method() == HTTP_GET) {
      std::string url_str = url;
      std::string query = get_query_string(request);
      ESP_LOGI(TAG, "route GET url=%s query_len=%u", url_str.c_str(), static_cast<unsigned>(query.size()));
      if (!query.empty()) {
        ESP_LOGD(TAG, "full url: %s?%s", url_str.c_str(), query.c_str());
        ESP_LOGD(TAG, "query: %s", query.c_str());
        if (query.find("delete=1") != std::string::npos || query == "delete" ||
            query.find("delete=true") != std::string::npos) {
          ESP_LOGI(TAG, "delete requested via query: %s?%s", url_str.c_str(), query.c_str());
          this->handle_delete(request);
          return;
        }
      } else {
        ESP_LOGD(TAG, "full url: %s", url_str.c_str());
      }
      this->handle_get(request);
      ESP_LOGI(TAG, "route GET done url=%s", url.c_str());
      return;
    }
    if (request->method() == HTTP_DELETE) {
      ESP_LOGI(TAG, "delete requested via HTTP DELETE: %s", url.c_str());
      this->handle_delete(request);
      ESP_LOGI(TAG, "route DELETE done url=%s", url.c_str());
      return;
    }
    if (request->method() == HTTP_PUT || request->method() == HTTP_POST) {
      httpd_req_t *req = *request;
      if (this->upload_in_progress_) {
        ESP_LOGW(TAG, "upload blocked (another upload in progress): %s", url.c_str());
        send_plain_response(req, 503, "{ \"error\": \"upload already in progress\" }");
        return;
      }
      if (!this->upload_enabled_) {
        ESP_LOGW(TAG, "upload blocked (upload disabled): %s", url.c_str());
        send_plain_response(req, 401, "{ \"error\": \"file upload is disabled\" }");
        return;
      }
      if (this->sd_spi_card_ == nullptr && this->sd_mmc_card_ == nullptr) {
        ESP_LOGE(TAG, "upload failed (sd not configured): %s", url.c_str());
        send_plain_response(req, 500, "{ \"error\": \"sd card not configured\" }");
        return;
      }
#if HAVE_SD_SPI_CARD
      if (this->sd_spi_card_ != nullptr) {
        if (!this->sd_spi_card_->is_mounted()) {
          if (!this->sd_spi_card_->mount()) {
            ESP_LOGE(TAG, "upload failed (sd spi not mounted): %s", url.c_str());
            send_plain_response(req, 500, "{ \"error\": \"sd card not mounted\" }");
            return;
          }
        }
      } else
#endif
      if (this->sd_mmc_card_ != nullptr) {
        if (!this->sd_mmc_card_->is_mounted()) {
          if (!this->sd_mmc_card_->mount()) {
            ESP_LOGE(TAG, "upload failed (sd mmc not mounted): %s", url.c_str());
            send_plain_response(req, 500, "{ \"error\": \"sd card not mounted\" }");
            return;
          }
        }
      }

      std::string extracted = this->extract_path_from_url(url);
      if (extracted.empty() || extracted.back() == '/') {
        ESP_LOGW(TAG, "upload missing filename: %s", url.c_str());
        send_plain_response(req, 400, "{ \"error\": \"missing filename\" }");
        return;
      }
      std::string path = this->build_absolute_path(extracted);
      size_t remaining = req->content_len;
      if (remaining == 0) {
        ESP_LOGW(TAG, "upload missing content-length: %s", request->url_to(buffer).str().c_str());
        send_plain_response(req, 411, "{ \"error\": \"content length required\" }");
        return;
      }

      std::string query = get_query_string(request);
      bool append_mode = !query.empty() && (query.find("append=1") != std::string::npos ||
                                            query.find("append=true") != std::string::npos);

      ESP_LOGI(TAG, "upload start: %s (%u bytes)%s", path.c_str(), static_cast<unsigned>(remaining),
               append_mode ? " [append]" : "");

      httpd_req_t *async_req = nullptr;
      esp_err_t async_err = httpd_req_async_handler_begin(req, &async_req);
      if (async_err != ESP_OK || async_req == nullptr) {
        ESP_LOGW(TAG, "upload failed to start async handler: %d", (int) async_err);
        send_plain_response(req, 500, "{ \"error\": \"async handler begin failed\" }");
        return;
      }

      this->upload_in_progress_ = true;

#if HAVE_SD_SPI_CARD
      if (this->sd_spi_card_ != nullptr && !append_mode) {
        this->sd_spi_card_->delete_file(path);
      } else
#endif
      if (this->sd_mmc_card_ != nullptr && !append_mode) {
        this->sd_mmc_card_->delete_file(path);
      }

      auto *ctx = new UploadTaskContext{this, async_req, path, append_mode};
      if (xTaskCreate(&SDFileServer::upload_task, "sd_upload", 8192, ctx, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "upload failed to start task");
        send_plain_response(async_req, 500, "{ \"error\": \"upload task create failed\" }");
        httpd_req_async_handler_complete(async_req);
        this->upload_in_progress_ = false;
        delete ctx;
      }
      return;
    }
  }
}

void SDFileServer::handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,
                                uint8_t *data, size_t len, bool final) {
  char buffer[513];
  if (!this->upload_enabled_) {
    ESP_LOGW(TAG, "upload blocked (upload disabled): %s", request->url_to(buffer).str().c_str());
    request->send(401, "application/json", "{ \"error\": \"file upload is disabled\" }");
    return;
  }
  if (this->sd_spi_card_ == nullptr && this->sd_mmc_card_ == nullptr) {
    ESP_LOGE(TAG, "upload failed (sd not configured): %s", request->url_to(buffer).str().c_str());
    request->send(500, "application/json", "{ \"error\": \"sd card not configured\" }");
    return;
  }
  std::string extracted = this->extract_path_from_url(request->url_to(buffer).str());
  std::string path = this->build_absolute_path(extracted);

  // Build absolute file path
  std::string file_name = filename;
  std::string file_path = Path::join(path, file_name);
  if (index == 0) {
    ESP_LOGD(TAG, "uploading file %s to %s", file_name.c_str(), path.c_str());
    ESP_LOGI(TAG, "upload start: %s (%u bytes)", file_path.c_str(), static_cast<unsigned>(len));
    // remove existing file (ignore result) then start first chunk
#if HAVE_SD_SPI_CARD
    if (this->sd_spi_card_ != nullptr) {
      this->sd_spi_card_->delete_file(file_path);
      this->sd_spi_card_->append_file_chunk(file_path, data, len, true);
      return;
    }
#endif
    if (this->sd_mmc_card_ != nullptr) {
      this->sd_mmc_card_->delete_file(file_path);
      this->sd_mmc_card_->append_file_chunk(file_path, data, len, true);
    }
    return;
  }
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    this->sd_spi_card_->append_file_chunk(file_path, data, len, false);
  } else
#endif
  if (this->sd_mmc_card_ != nullptr) {
    this->sd_mmc_card_->append_file_chunk(file_path, data, len, false);
  }
  if (final) {
    ESP_LOGI(TAG, "upload complete: %s", file_path.c_str());
    request->send(200, "text/plain", "upload success");
    return;
  }
}

void SDFileServer::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                              size_t total) {
  char buffer[513];
  if (request->method() != HTTP_PUT && request->method() != HTTP_POST) return;
  if (!this->upload_enabled_) {
    if (index == 0) {
      ESP_LOGW(TAG, "raw upload blocked (upload disabled): %s", request->url_to(buffer).str().c_str());
      request->send(401, "application/json", "{ \"error\": \"file upload is disabled\" }");
    }
    return;
  }
  if (this->sd_spi_card_ == nullptr && this->sd_mmc_card_ == nullptr) {
    if (index == 0) {
      ESP_LOGE(TAG, "raw upload failed (sd not configured): %s", request->url_to(buffer).str().c_str());
      request->send(500, "application/json", "{ \"error\": \"sd card not configured\" }");
    }
    return;
  }
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    if (!this->sd_spi_card_->is_mounted()) {
      if (!this->sd_spi_card_->mount()) {
        if (index == 0) {
          ESP_LOGE(TAG, "raw upload failed (sd spi not mounted): %s", request->url_to(buffer).str().c_str());
          request->send(500, "application/json", "{ \"error\": \"sd card not mounted\" }");
        }
        return;
      }
    }
  } else
#endif
  if (this->sd_mmc_card_ != nullptr) {
    if (!this->sd_mmc_card_->is_mounted()) {
      if (!this->sd_mmc_card_->mount()) {
        if (index == 0) {
          ESP_LOGE(TAG, "raw upload failed (sd mmc not mounted): %s", request->url_to(buffer).str().c_str());
          request->send(500, "application/json", "{ \"error\": \"sd card not mounted\" }");
        }
        return;
      }
    }
  }

  std::string extracted = this->extract_path_from_url(request->url_to(buffer).str());
  if (extracted.empty() || extracted.back() == '/') {
    if (index == 0) {
      ESP_LOGW(TAG, "raw upload missing filename: %s", request->url_to(buffer).str().c_str());
      request->send(400, "application/json", "{ \"error\": \"missing filename\" }");
    }
    return;
  }

  std::string path = this->build_absolute_path(extracted);
  if (index == 0) {
    ESP_LOGI(TAG, "raw upload start: %s (%u bytes)", path.c_str(), static_cast<unsigned>(total));
#if HAVE_SD_SPI_CARD
    if (this->sd_spi_card_ != nullptr) {
      this->sd_spi_card_->delete_file(path);
      this->sd_spi_card_->append_file_chunk(path, data, len, true);
    } else
#endif
    if (this->sd_mmc_card_ != nullptr) {
      this->sd_mmc_card_->delete_file(path);
      this->sd_mmc_card_->append_file_chunk(path, data, len, true);
    }
  } else {
#if HAVE_SD_SPI_CARD
    if (this->sd_spi_card_ != nullptr) {
      this->sd_spi_card_->append_file_chunk(path, data, len, false);
    } else
#endif
    if (this->sd_mmc_card_ != nullptr) {
      this->sd_mmc_card_->append_file_chunk(path, data, len, false);
    }
  }

  if (index + len >= total) {
    ESP_LOGI(TAG, "raw upload complete: %s", path.c_str());
    request->send(200, "text/plain", "upload success");
  }
}

void SDFileServer::set_url_prefix(std::string const &prefix) { this->url_prefix_ = prefix; }

void SDFileServer::set_root_path(std::string const &path) { this->root_path_ = path; }

void SDFileServer::set_sd_mmc_card(::esphome::sd_card::SdMmcCard *card) { this->sd_mmc_card_ = card; }

void SDFileServer::set_sd_spi_card(::esphome::sd_card::SdSpiCard *card) { this->sd_spi_card_ = card; }

void SDFileServer::set_deletion_enabled(bool allow) { this->deletion_enabled_ = allow; }

void SDFileServer::set_download_enabled(bool allow) { this->download_enabled_ = allow; }

void SDFileServer::set_upload_enabled(bool allow) { this->upload_enabled_ = allow; }

void SDFileServer::handle_get(AsyncWebServerRequest *request) const {
  char buffer[513];
  std::string extracted = this->extract_path_from_url(request->url_to(buffer).str());
  std::string path = this->build_absolute_path(extracted);
  ESP_LOGD(TAG, "GET %s -> %s", request->url_to(buffer).str().c_str(), path.c_str());

  if (this->sd_spi_card_ == nullptr && this->sd_mmc_card_ == nullptr) {
    request->send(500, "application/json", "{ \"error\": \"sd card not configured\" }");
    return;
  }
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    if (!this->sd_spi_card_->is_mounted()) {
      if (!this->sd_spi_card_->mount()) {
        request->send(500, "application/json", "{ \"error\": \"sd card not mounted\" }");
        return;
      }
    }
  } else
#endif
  if (this->sd_mmc_card_ != nullptr) {
    if (!this->sd_mmc_card_->is_mounted()) {
      if (!this->sd_mmc_card_->mount()) {
        request->send(500, "application/json", "{ \"error\": \"sd card not mounted\" }");
        return;
      }
    }
  }

  if (extracted.empty() || extracted == "/") {
    ESP_LOGI(TAG, "listing root: %s", path.c_str());
    handle_index(request, path);
    return;
  }

  bool is_dir = false;
  bool exists = false;
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    exists = this->sd_spi_card_->stat_file(path, nullptr, &is_dir);
  } else
#endif
  if (this->sd_mmc_card_ != nullptr) {
    exists = this->sd_mmc_card_->stat_file(path, nullptr, &is_dir);
  }
  if (exists) {
    if (!is_dir) {
      ESP_LOGI(TAG, "download file: %s", path.c_str());
      handle_download(request, path);
      return;
    }
    ESP_LOGI(TAG, "listing dir: %s", path.c_str());
    handle_index(request, path);
    return;
  }

  request->send(404, "application/json", "{ \"error\": \"not found\" }");
}

// Previously this component tried to render an HTML table using a FileInfo
// structure. The current sd SPI shim exposes a JSON directory listing and
// simple read/append/delete helpers. For now, render the raw JSON listing
// from the sd card shim to keep the feature working without depending on
// a FileInfo type.
void SDFileServer::handle_index(AsyncWebServerRequest *request, std::string const &path) const {
  ESP_LOGI(TAG, "listing path: %s", path.c_str());
#if HAVE_SD_SPI_CARD
  std::string json = this->sd_spi_card_ != nullptr ? this->sd_spi_card_->list_dir_json(path)
                                                   : this->sd_mmc_card_->list_dir_json(path);
#else
  std::string json = this->sd_mmc_card_->list_dir_json(path);
#endif
  std::string query = get_query_string(request);
  if (query.find("json=1") != std::string::npos || query.find("format=json") != std::string::npos) {
    request->send(200, "application/json", json.c_str());
    return;
  }

  AsyncResponseStream *response = request->beginResponseStream("text/html");
  response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  response->addHeader("Pragma", "no-cache");
  response->print("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>SD Browser</title>");
  response->print("<style>body{font-family:Segoe UI,Arial,sans-serif;margin:16px;background:#f7f8fa;color:#111}h2{margin:0 0 12px 0}table{border-collapse:collapse;width:100%;background:#fff}th,td{padding:8px 10px;border-bottom:1px solid #e5e7eb;text-align:left}th{background:#f3f4f6}a{text-decoration:none;color:#0b63ce}a:hover{text-decoration:underline}.path{margin:8px 0 14px 0;color:#374151;font-size:14px}.muted{color:#6b7280}</style>");
  response->print("</head><body><h2>SD File Browser</h2><div class=\"path\" id=\"path\"></div><table><thead><tr><th>Name</th><th>Type</th><th>Size</th><th>Action</th></tr></thead><tbody id=\"rows\"></tbody></table>");
  response->print("<script>(function(){const esc=(s)=>String(s).replace(/[&<>\"]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[m]));const path=location.pathname;document.getElementById('path').textContent='Path: '+path;const rows=document.getElementById('rows');const sep=path.includes('?')?'&':'?';fetch(path+sep+'json=1',{cache:'no-store'}).then(r=>r.json()).then(items=>{if(path!=='/file/'&&path!=='/file'){const p=path.endsWith('/')?path.slice(0,-1):path;const i=p.lastIndexOf('/');const up=(i>0?p.slice(0,i+1):'/file/');rows.insertAdjacentHTML('beforeend','<tr><td><a href=\"'+up+'\">..</a></td><td class=\"muted\">dir</td><td>-</td><td>-</td></tr>');}for(const it of items){const isDir=!!it.is_dir;const name=String(it.name||'');const href=(path.endsWith('/')?path:path+'/')+encodeURIComponent(name)+(isDir?'/':'');rows.insertAdjacentHTML('beforeend','<tr><td><a href=\"'+href+'\">'+esc(name)+(isDir?'/':'')+'</a></td><td>'+(isDir?'dir':'file')+'</td><td>'+(isDir?'-':String(it.size||0))+'</td><td>'+(isDir?'-':'<a href=\"'+href+'\" download>download</a>')+'</td></tr>');}}).catch(e=>{rows.innerHTML='<tr><td colspan=\"4\">Failed to load directory: '+esc(e)+'</td></tr>';});})();</script>");
  response->print("</body></html>");
  request->send(response);
}

// handle_index now implemented above (replaced earlier implementation).

void SDFileServer::handle_download(AsyncWebServerRequest *request, std::string const &path) const {
  if (!this->download_enabled_) {
    ESP_LOGW(TAG, "download blocked (download disabled): %s", path.c_str());
    request->send(401, "application/json", "{ \"error\": \"file download is disabled\" }");
    return;
  }

  ESP_LOGI(TAG, "download start: %s", path.c_str());

  httpd_req_t *req = *request;
  httpd_req_t *async_req = nullptr;
  esp_err_t async_err = httpd_req_async_handler_begin(req, &async_req);
  if (async_err != ESP_OK || async_req == nullptr) {
    ESP_LOGW(TAG, "download failed to start async handler: %d", (int) async_err);
    send_plain_response(req, 500, "{ \"error\": \"async handler begin failed\" }");
    return;
  }

  std::string mime = Path::mime_type(path);
  httpd_resp_set_status(async_req, HTTPD_200);
  httpd_resp_set_type(async_req, mime.c_str());

  auto *ctx = new DownloadTaskContext{this, async_req, path};
  if (xTaskCreate(&SDFileServer::download_task, "sd_download", 8192, ctx, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    ESP_LOGW(TAG, "download failed to start task");
    send_plain_response(async_req, 500, "{ \"error\": \"download task create failed\" }");
    httpd_req_async_handler_complete(async_req);
    delete ctx;
  }
}

std::string SDFileServer::build_prefix() const {
  if (this->url_prefix_.length() == 0 || this->url_prefix_.at(0) != '/')
    return "/" + this->url_prefix_;
  return this->url_prefix_;
}

std::string SDFileServer::extract_path_from_url(std::string const &url) const {
  std::string prefix = this->build_prefix();
  std::string path = url.substr(prefix.size(), url.size() - prefix.size());
  auto qpos = path.find('?');
  if (qpos != std::string::npos) {
    path = path.substr(0, qpos);
  }
  return path;
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
  char buffer[513];
  if (!this->deletion_enabled_) {
    ESP_LOGW(TAG, "delete blocked (deletion disabled): %s", request->url_to(buffer).str().c_str());
    request->send(401, "application/json", "{ \"error\": \"deletion disabled\" }");
    return;
  }
  if (this->sd_spi_card_ == nullptr && this->sd_mmc_card_ == nullptr) {
    ESP_LOGE(TAG, "delete failed (sd not configured): %s", request->url_to(buffer).str().c_str());
    request->send(500, "application/json", "{ \"error\": \"sd card not configured\" }");
    return;
  }
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    if (!this->sd_spi_card_->is_mounted()) {
      ESP_LOGW(TAG, "sd spi not mounted, mounting before delete");
      if (!this->sd_spi_card_->mount()) {
        ESP_LOGE(TAG, "delete failed (sd spi mount failed)");
        request->send(500, "application/json", "{ \"error\": \"sd card not mounted\" }");
        return;
      }
    }
  } else
#endif
  if (this->sd_mmc_card_ != nullptr) {
    if (!this->sd_mmc_card_->is_mounted()) {
      ESP_LOGW(TAG, "sd mmc not mounted, mounting before delete");
      if (!this->sd_mmc_card_->mount()) {
        ESP_LOGE(TAG, "delete failed (sd mmc mount failed)");
        request->send(500, "application/json", "{ \"error\": \"sd card not mounted\" }");
        return;
      }
    }
  }
  std::string url = request->url_to(buffer).str();
  std::string extracted = this->extract_path_from_url(url);
  std::string query = get_query_string(request);
  std::string query_path;
  if (!query.empty()) {
    query_path = query_param_value(query, "path");
  }
  if (!query_path.empty() && query_path.front() != '/') {
    query_path = "/" + query_path;
  }
  std::string target = !query_path.empty() ? query_path : extracted;
  std::string path = this->build_absolute_path(target);
  ESP_LOGI(TAG, "deleting path: %s (extracted: %s, query: %s)", path.c_str(), extracted.c_str(), query_path.c_str());
  bool ok = false;
#if HAVE_SD_SPI_CARD
  if (this->sd_spi_card_ != nullptr) {
    ok = this->sd_spi_card_->delete_file(path);
  } else
#endif
  if (this->sd_mmc_card_ != nullptr) {
    ok = this->sd_mmc_card_->delete_file(path);
  }
  if (!ok) {
    ESP_LOGW(TAG, "delete failed: %s", path.c_str());
    request->send(500, "application/json", "{ \"error\": \"failed to delete\" }");
    return;
  }
  ESP_LOGI(TAG, "delete success: %s", path.c_str());
  request->send(200, "application/json", "{ \"success\": true }");
}

}  // namespace sd_file_server
}  // namespace esphome