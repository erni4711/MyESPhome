#include "web_admin_local.h"
// The generated/web_admin_assets.h header (produced by generate-web-assets.mjs)
// contains the same declarations as the inline block tagged GENERATED-META below.
// Include it opportunistically so IDEs and local builds get the file-backed version.
#if __has_include("generated/web_admin_assets.h")
#include "generated/web_admin_assets.h"
#endif

#include <algorithm>

// BEGIN_GENERATED_META — managed by tools/generate-web-assets.mjs, do not edit
// Wrapped in #ifndef so it is skipped when generated/web_admin_assets.h is included.
#ifndef WEB_ADMIN_ASSETS_META_H
namespace web_admin_assets_generated {

inline constexpr char kAdminCssPath[] =
    "/admin/assets/admin.bfb128b671f2.css";
inline constexpr char kAdminCssEtag[] =
    "\"2fbbc18f2f100c597721426330794f8c06e4f3f62b0de25649a422c93aa64f66\"";
inline constexpr char kAdminCssContentType[] =
    "text/css; charset=utf-8";
inline constexpr size_t kAdminCssSourceSize = 77372;
inline constexpr size_t kAdminCssGzipSize = 13791;

inline constexpr char kAdminJsPath[] =
    "/admin/assets/admin.b1231748a46f.js";
inline constexpr char kAdminJsEtag[] =
    "\"2d361f0599cdd3b6b9ed29eeb7a17a86adfb8ba2a6b20edbee6b97f351afac2a\"";
inline constexpr char kAdminJsContentType[] =
    "application/javascript; charset=utf-8";
inline constexpr size_t kAdminJsSourceSize = 438160;
inline constexpr size_t kAdminJsGzipSize = 86341;

}  // namespace web_admin_assets_generated
#endif  // WEB_ADMIN_ASSETS_META_H
// END_GENERATED_META

namespace {

static const uint8_t kAdminCssGzip[] PROGMEM = {
#include "generated/admin_css_gzip.inc"
};

static const uint8_t kAdminJsGzip[] PROGMEM = {
#include "generated/admin_js_gzip.inc"
};

static_assert(
    sizeof(kAdminCssGzip) == web_admin_assets_generated::kAdminCssGzipSize,
    "Generated admin CSS size mismatch");
static_assert(
    sizeof(kAdminJsGzip) == web_admin_assets_generated::kAdminJsGzipSize,
    "Generated admin JavaScript size mismatch");

struct GzipWebAsset {
  const uint8_t* data;
  size_t size;
  const char* content_type;
  const char* etag;
};

// void sendGzipProgmemAsset(WebServer& server,
//                           const GzipWebAsset& asset,
//                           size_t chunk_size = 512) {
//   if (chunk_size < 256) chunk_size = 256;

//   server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
//   server.sendHeader("ETag", asset.etag);
//   server.sendHeader("Vary", "Accept-Encoding");
//   server.sendHeader("Content-Encoding", "gzip");
//   server.sendHeader("X-Content-Type-Options", "nosniff");
//   // A 304 may carry the Content-Length the corresponding 200 response would
//   // have had. WebServer otherwise emits an incorrect zero length here.
//   server.setContentLength(asset.size);

//   const String if_none_match = server.header("If-None-Match");
//   if (if_none_match == asset.etag || if_none_match == "*") {
//     server.send(304, asset.content_type, "");
//     return;
//   }

//   server.send(200, asset.content_type, "");

//   for (size_t offset = 0; offset < asset.size; offset += chunk_size) {
//     const size_t length = std::min(chunk_size, asset.size - offset);
//     server.sendContent_P(
//         reinterpret_cast<PGM_P>(asset.data + offset), length);
//     // Keep the proven ESP-Hosted pacing. Compression makes the total pause
//     // much shorter without increasing SDIO pressure per write.
//     delay(2);
//     yield();
//   }
// }

}  // namespace

const char* adminCssAssetPath() {
  return web_admin_assets_generated::kAdminCssPath;
}

const char* adminJsAssetPath() {
  return web_admin_assets_generated::kAdminJsPath;
}


namespace web_admin_local {

void LocalHandler::sendAdminCssAsset(AsyncWebServerRequest *request)
{
  const GzipWebAsset asset{
      kAdminCssGzip,
      sizeof(kAdminCssGzip),
      web_admin_assets_generated::kAdminCssContentType,
      web_admin_assets_generated::kAdminCssEtag};
  auto *res = request->beginResponse(200, asset.content_type, asset.data, asset.size);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "public, max-age=3600");
  request->send(res);
}
void LocalHandler::sendAdminJsAsset(AsyncWebServerRequest *request)
{
  const GzipWebAsset asset{
      kAdminJsGzip,
      sizeof(kAdminJsGzip),
      web_admin_assets_generated::kAdminJsContentType,
      web_admin_assets_generated::kAdminJsEtag};
  auto *res = request->beginResponse(200, asset.content_type, asset.data, asset.size);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "public, max-age=3600");
  request->send(res);
}

}  // namespace web_admin_local
