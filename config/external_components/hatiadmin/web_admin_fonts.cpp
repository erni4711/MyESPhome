#include "web_admin_fonts.h"
#include "web_admin_local.h"

namespace {

// Latin subset of Inter 4.1. The matching OFL-1.1 license is stored in
// src/fonts/Inter-LICENSE.txt.
static const uint8_t kInterRegularWoff2[] = {
#include "generated/inter_4_1_regular_woff2.inc"
};

static const uint8_t kInterSemiboldWoff2[] = {
#include "generated/inter_4_1_semibold_woff2.inc"
};


}  // namespace

void appendWebFontFaceStyles(std::string& html) {
  html += R"html(
  <style>
    @font-face {
      font-family:'HomeTiles Inter';
      src:url('/assets/inter-4.1-regular.woff2') format('woff2');
      font-style:normal;
      font-weight:400;
      font-display:swap;
    }
    @font-face {
      font-family:'HomeTiles Inter';
      src:url('/assets/inter-4.1-regular.woff2') format('woff2');
      font-style:normal;
      font-weight:500;
      font-display:swap;
    }
    @font-face {
      font-family:'HomeTiles Inter';
      src:url('/assets/inter-4.1-semibold.woff2') format('woff2');
      font-style:normal;
      font-weight:600;
      font-display:swap;
    }
    @font-face {
      font-family:'HomeTiles Inter';
      src:url('/assets/inter-4.1-semibold.woff2') format('woff2');
      font-style:normal;
      font-weight:700;
      font-display:swap;
    }
  </style>
)html";
}

namespace web_admin_local {

void LocalHandler::sendWebFontRegular(AsyncWebServerRequest *request)
{
  auto *res = request->beginResponse(200, "font/woff2", kInterRegularWoff2, sizeof(kInterRegularWoff2));
  request->send(res);
}
void LocalHandler::sendWebFontSemibold(AsyncWebServerRequest *request)
{
  auto *res = request->beginResponse(200, "font/woff2", kInterSemiboldWoff2, sizeof(kInterSemiboldWoff2));
  request->send(res);
}

}  // namespace web_admin_local


// No runtime font send handlers are required for compilation testing.
