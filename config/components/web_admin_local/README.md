# web_admin_local build and flash instructions

These instructions use the global Python environment and the `esphome`
command-line application. They do not require calling ESP-IDF commands
directly.

## Compile

From the repository root, compile the P4-10 sample configuration:

```powershell
esphome compile config\P4-10-sample2.yaml
```

The generated firmware is written to:

```text
config\.esphome\build\esp32p4-10-sample2\build\esp32p4-10-sample2.bin
```

During code generation, `web_admin_local` regenerates its compressed web
assets and copies the LVGL tile, font, and MDI icon sources into the active
ESPHome build directory.

## Flash over OTA

Use the device hostname when mDNS is available:

```powershell
esphome upload config\P4-10-sample2.yaml --device esp32p4-10-sample2.local
```

Or use the device IP address:

```powershell
esphome upload config\P4-10-sample2.yaml --device 192.168.10.26
```

`esphome run` can compile and upload in one step:

```powershell
esphome run config\P4-10-sample2.yaml --device 192.168.10.26
```

## Flash over USB

If OTA is unavailable, connect the board over USB and let ESPHome select the
serial port:

```powershell
esphome upload config\P4-10-sample2.yaml
```

To select a specific port, add `--device` with the port shown by Windows,
for example:

```powershell
esphome upload config\P4-10-sample2.yaml --device COM7
```

## Verify the web admin

After the device reconnects, check the admin page and tile API:

```powershell
curl.exe -I http://192.168.10.26/admin
curl.exe http://192.168.10.26/admin/folders
curl.exe "http://192.168.10.26/admin/tiles?folder=0"
```

`/admin/entity_options` reads Home Assistant entities from `GET /api/states`.
Configure the Home Assistant URL and a long-lived access token on
`web_admin_local`, keeping the token in `secrets.yaml`:

```yaml
web_admin_local:
  web_server_base_id: my_web_server_base
  url_prefix: admin
  home_assistant_url: "http://homeassistant.local:8123"
  home_assistant_token: !secret home_assistant_long_lived_token
```

The endpoint returns HTTP 503 when the REST settings are absent and HTTP 502
when Home Assistant cannot be reached or returns invalid JSON.

## Home Assistant websocket live updates

In addition to the REST calls above (used for switch toggles and the entity
picker), `web_admin_local` opens a persistent `ws://` or `wss://` connection
to `<home_assistant_url>/api/websocket` (see `ha_ws_client.h`/`.cpp`) using
the same `home_assistant_url` / `home_assistant_token` settings. It:

- Authenticates with the configured long-lived access token (never logged).
- Sends `subscribe_events` for `state_changed`, then `get_states` to seed
  initial values (best effort — very large Home Assistant installs may
  exceed the buffered message size limit and are skipped; the next matching
  `state_changed` event still updates the tile).
- Filters incoming state updates to only the `sensor_entity` / `energy_entity`
  ids actually referenced by a stored tile grid, and only ever applies them
  to LVGL labels/gauges from the ESPHome `loop()` task (never from the
  websocket client's own task).
- Uses the ESP-IDF-managed `espressif/esp_websocket_client` component (see
  `__init__.py`); no PlatformIO/Arduino library is required.

`wss://` connections skip full certificate verification (via
`CONFIG_ESP_TLS_INSECURE` / `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`) since
local Home Assistant instances commonly use a self-signed certificate.
Existing switch tiles are unaffected — they still use the REST
`turn_on`/`turn_off` calls in `tiles_lvgl.cpp`.

The tile round-trip integration test uses the global Python environment:

```powershell
python config\components\web_admin_local\tests\test_web_admin_local_tiles.py `
  --url http://192.168.10.26/admin/tiles `
  --file config\components\web_admin_local\tests\waveshare_tiles_2026-08-18T17-06-44-970Z.json
```

The test uploads and reads back every grid in the fixture. A successful run
ends with `Results: 2/2 folders passed`.

## Updating web assets only

When changing `assets\admin.js` or `assets\admin.css`, regenerate the embedded
assets before compiling:

```powershell
cd config\components\web_admin_local
node tools\generate-web-assets.mjs
cd ..\..\..
```
