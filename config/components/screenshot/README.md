# Screenshot component for ESPHome

A small ESPHome custom component that captures the LVGL display framebuffer and serves it as a PNG image via the device web server. Useful for debugging displays, generating documentation images, or exposing device screen state to Home Assistant automations.

## Authors
Written with the support of ChatGPT.
PNG output is based on [PNGenc](https://github.com/bitbank2/PNGenc).

## Requirements
- ESPHome 2026.4.0 or later
- LVGL 9.5 or later
- `web_server_base` configured on the device

## Installation
Place the `screenshot/` folder in your ESPHome config directory under `components/`:

```
config/
  components/
    screenshot/
      __init__.py
      screenshot.cpp
      screenshot.h
      ...
```

## Configuration

```yaml
web_server_base:
  id: my_web_server_base

screenshot:
  id: screenshot_component
  sd_mmc_id: sdcard          # optional: save to SD card
  # camera_id: ov5647_camera # optional: capture camera frame instead of display
```

See [ESP32-P4-WIFI6-Touch-LCD-7B.yaml](../boards/ESP32-P4-WIFI6-Touch-LCD-7B.yaml) for a full working example.

## Usage

Once the device is running, capture the current display with a simple HTTP GET:

```bash
# Save to file
curl http://<device-ip>/screenshot.png -o screenshot.png

# PowerShell
Invoke-WebRequest -Uri "http://<device-ip>/screenshot.png" -OutFile "screenshot.png"
```

The endpoint returns a PNG image of the current LVGL screen. If a capture is already in progress it returns HTTP 503 with a JSON body:
```json
{ "ready": false, "in_progress": true, "saved": false, "message": "Capture queued; retry in a moment" }
```
Retry after 2–3 seconds.

### Capturing per-page screenshots

To capture each LVGL page separately, switch pages via the **LVGL Page** select entity in Home Assistant, then request the screenshot endpoint:

1. In HA, set the `select.<device>_lvgl_page` entity to the desired page (e.g. `Climate`).
2. Wait ~1 second for the display to render.
3. `GET http://<device-ip>/screenshot.png`

Repeat for each page.

> **Note:** The ESPHome web server v2 (default since 2024.x) uses a WebSocket-based API — direct REST calls to `/select/...` are not supported. Use Home Assistant or the ESPHome dashboard to switch pages.

## License
Apache — see LICENSE file for details.