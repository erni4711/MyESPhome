#include "web_admin_local.h"
#include "web_admin_fonts.h"

// Helper to call the project's appendWebFontFaceStyles which uses ESPHome's
// String type. Convert between std::string and String here.
// Simple wrapper that forwards to the project's appendWebFontFaceStyles
static void appendWebFontFaceStylesStd(std::string &html) {
  appendWebFontFaceStyles(html);
}

namespace web_admin_local {

void LocalHandler::handleRoot(AsyncWebServerRequest* request) {
  request->send(200, "text/html; charset=utf-8", getConfigPage().c_str());
}

std::string LocalHandler::getConfigPage() {
  // Minimal, self-contained config page for ESPHome build integration.
  const std::string ap_page_title = "HomeTiles WiFi Configuration";

  std::string html = "<!DOCTYPE html>\n<html lang=\"en\">\n";
  html += R"html(<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>)html";
  html += ap_page_title;
  html += R"html(</title>
  <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48'%3E%3Crect width='48' height='48' rx='10' fill='%2316181c'/%3E%3Crect x='4' y='4' width='17' height='17' rx='4' fill='%23ffffff'/%3E%3Crect x='27' y='4' width='17' height='17' rx='4' fill='%23ffffff'/%3E%3Crect x='4' y='27' width='17' height='17' rx='4' fill='%23ffffff'/%3E%3Cpath d='M33 26h5v6.5h6.5v5H38V44h-5v-6.5h-6.5v-5H33z' fill='%2326a69a'/%3E%3C/svg%3E">
)html";
  appendWebFontFaceStylesStd(html);
  html += R"html(
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'HomeTiles Inter', sans-serif;
      background: #0a0a0a;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .container {
      background: #1c1c1c;
      border: 1px solid #2a2a2a;
      border-radius: 22px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.5);
      max-width: 420px;
      width: 100%;
      padding: 32px;
    }
    .brand {
      display: flex;
      align-items: center;
      gap: 14px;
      margin-bottom: 28px;
    }
    .brand h1 {
      color: #ffffff;
      font-size: 22px;
    }
    .brand .device {
      color: #8a8a8a;
      font-size: 13px;
      margin-top: 2px;
    }
    .form-group {
      margin-bottom: 16px;
    }
    label {
      display: block;
      color: #b8b8b8;
      font-size: 14px;
      font-weight: 500;
      margin-bottom: 6px;
    }
    input {
      width: 100%;
      padding: 12px 16px;
      border: 1px solid #333333;
      border-radius: 12px;
      background: #141414;
      color: #ffffff;
      font-size: 15px;
      transition: border-color 0.2s;
      font-family: inherit;
    }
    input:focus {
      outline: none;
      border-color: #26a69a;
    }
    input::placeholder {
      color: #666666;
    }
    .password-field {
      display: flex;
      gap: 8px;
      align-items: center;
    }
    .password-field input {
      flex: 1 1 auto;
    }
    .password-toggle {
      flex: 0 0 auto;
      padding: 12px 14px;
      border: 1px solid #333333;
      border-radius: 12px;
      background: #141414;
      color: #b8b8b8;
      font-size: 13px;
      font-weight: 600;
      cursor: pointer;
    }
    .hint {
      color: #666666;
      font-size: 12px;
      margin-top: 4px;
    }
    .btn {
      width: 100%;
      padding: 14px;
      /* Gruen wie alle "Los"-Aktionen (Verbinden/Speichern/Update) - Tuerkis
         bleibt Akzentfarbe fuer Fokus-Ringe und Logo. */
      background: #2e7d32;
      color: white;
      border: none;
      border-radius: 12px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      margin-top: 8px;
    }
    .btn:active {
      background: #1b5e20;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="brand">
      <svg width="48" height="48" viewBox="0 0 48 48" xmlns="http://www.w3.org/2000/svg" style="flex-shrink:0;" aria-hidden="true">
        <rect x="4" y="4" width="17" height="17" rx="4" fill="#ffffff"/>
        <rect x="27" y="4" width="17" height="17" rx="4" fill="#ffffff"/>
        <rect x="4" y="27" width="17" height="17" rx="4" fill="#ffffff"/>
        <path d="M33 26h5v6.5h6.5v5H38V44h-5v-6.5h-6.5v-5H33z" fill="#26a69a"/>
      </svg>
      <div>
        <h1>HomeTiles</h1>
        <div class="device">)html";
  html += "HomeTiles";
  html += R"html(</div>
      </div>
    </div>

    <form action="save" method="POST">
      <div class="form-group">
        <label for="wifi_ssid">Network</label>
        <input type="text" id="wifi_ssid" name="wifi_ssid" placeholder="My WiFi" value=")html";
  html += "";
  html += R"html(" required>
      </div>
      <div class="form-group">
        <label for="wifi_pass">Password</label>
        <div class="password-field">
          <input type="password" id="wifi_pass" name="wifi_pass" placeholder="Password" value=")html";
  html += "";
  html += R"html(">
          <button type="button" class="password-toggle" onclick="togglePasswordVisibility('wifi_pass', this)">Show</button>
        </div>
        <div class="hint">Leave empty for an open network</div>
      </div>

      <button type="submit" class="btn">Connect</button>
    </form>
  </div>
  <script>
    function togglePasswordVisibility(inputId, buttonEl) {
      const input = document.getElementById(inputId);
      if (!input || !buttonEl) return;
      const isHidden = input.type === 'password';
      input.type = isHidden ? 'text' : 'password';
      buttonEl.textContent = isHidden ? 'Hide' : 'Show';
    }
  </script>
</body>
</html>
)html";

  return html;
}

std::string LocalHandler::getSuccessPage() {
  std::string html = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Connecting...</title>
  <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48'%3E%3Crect width='48' height='48' rx='10' fill='%2316181c'/%3E%3Crect x='4' y='4' width='17' height='17' rx='4' fill='%23ffffff'/%3E%3Crect x='27' y='4' width='17' height='17' rx='4' fill='%23ffffff'/%3E%3Crect x='4' y='27' width='17' height='17' rx='4' fill='%23ffffff'/%3E%3Cpath d='M33 26h5v6.5h6.5v5H38V44h-5v-6.5h-6.5v-5H33z' fill='%2326a69a'/%3E%3C/svg%3E">
)html";
  appendWebFontFaceStylesStd(html);
  html += R"html(
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'HomeTiles Inter', sans-serif;
      background: #0a0a0a;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .container {
      background: #1c1c1c;
      border: 1px solid #2a2a2a;
      border-radius: 22px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.5);
      max-width: 420px;
      width: 100%;
      padding: 32px;
      text-align: center;
    }
    .logo {
      margin: 0 auto 20px;
      width: 56px;
      height: 56px;
    }
    h1 {
      color: #ffffff;
      font-size: 22px;
      margin-bottom: 10px;
    }
    p {
      color: #8a8a8a;
      font-size: 14px;
      line-height: 1.6;
    }
  </style>
</head>
<body>
  <div class="container">
    <svg class="logo" viewBox="0 0 48 48" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
      <rect x="4" y="4" width="17" height="17" rx="4" fill="#ffffff"/>
      <rect x="27" y="4" width="17" height="17" rx="4" fill="#ffffff"/>
      <rect x="4" y="27" width="17" height="17" rx="4" fill="#ffffff"/>
      <path d="M33 26h5v6.5h6.5v5H38V44h-5v-6.5h-6.5v-5H33z" fill="#26a69a"/>
    </svg>
    <h1>Connecting...</h1>
    <p>HomeTiles is joining your network now.<br>This page will lose its connection - you can close it.</p>
  </div>
</body>
</html>
)html";
  return html;
}

}  // namespace web_admin_local
