---
name: hati-development
description: Develop and maintain HATi in MyESPhome, including hatiadmin, hatilvgl, hatifonts, hatidart, hatiserve, tile JSON compatibility, the embedded admin UI, Home Assistant REST/WebSocket integration, ESPHome configuration, builds, and device verification. Use for any HATi feature, bug fix, tile type, admin page, LVGL renderer, storage/API, font, media, animation, dart, or sample configuration change.
---

# HATi development

Use this skill for repository-specific HATi work. Keep the browser editor,
persisted JSON, firmware API, LVGL renderer, and ESPHome configuration aligned.

## Start with the relevant surfaces

- `config/external_components/hatiadmin/`
  - ESPHome codegen and the local `/admin` web application.
  - JSON handlers, folder/tile persistence, and embedded web assets.
- `config/external_components/hatiadmin/assets/admin.js`
  - Browser-side tile editor, validation, serialization, and live controls.
- `config/external_components/hatiadmin/assets/admin.css`
  - Admin UI styling.
- `config/external_components/hatiadmin/generated/`
  - Generated gzip includes and metadata. Never edit these by hand.
- `config/external_components/hatilvgl/`
  - Runtime tile parsing/rendering and Home Assistant REST/WebSocket updates.
- `config/external_components/hatifonts/`
  - LVGL fonts and Material Design icon mappings.
- `config/external_components/hatidart/`
  - Dart UI and persisted dart state.
- `config/external_components/hatiserve/`
  - SD card and SPIFFS HTTP file serving.
- `config/common/hati.yaml`
  - Shared component wiring.
- `config/P4-10-sample2.yaml`
  - Primary HATi sample used for configuration and compile validation.
- `config/external_components/hatiadmin/tools/backup/tiles-all.json`
  - Real multi-folder compatibility fixture. Preserve it unless the task
    explicitly asks to update the backup.

## Battery display pattern

The P4 10.1-inch board estimates battery percentage from the GPIO20 voltage
divider and publishes it through `hatilvgl_set_battery_level()`. The settings
tile uses a registered live MDI battery icon in the top-right corner instead
of its configured tile icon:

- Above 80%: `battery-90`, green.
- 30% through 80%: `battery-50`, white.
- Below 30% and at least 10%: `battery-30`, yellow.
- Below 10%: `battery-alert`, red.
- Unknown value: `battery-outline`, white.

Register dynamic battery labels or icons through the shared HATi helpers in
`hatilvgl.h`/`hatilvgl.cpp`. Remove registrations on `LV_EVENT_DELETE` so
folder rebuilds cannot retain stale LVGL pointers. Keep the existing battery
percentage label behavior when adding an icon.

Search for existing behavior before editing. HATi features commonly span more
than one component, and a change is incomplete if only the visible UI or only
the renderer is updated.

## Preserve tile compatibility

Tile type numbers are persisted API data. Never renumber or reuse an existing
number. Keep retired or reserved values intact.

When adding or changing a tile type, inspect and update every applicable
surface:

1. The tile type registry and generated HTML in
   `hatiadmin/tiles.cpp`.
2. Defaults, editor fields, load/save/reset handlers, validation, preview,
   import, and export behavior in `hatiadmin/assets/admin.js`.
3. Request parsing and JSON persistence in `hatiadmin/json_admin_handlers.cpp`,
   `hatiadmin/web_admin_local.cpp`, and related headers.
4. `TileData`, tile constants, JSON parsing, dispatch, entity collection, and
   runtime updates in `hatilvgl/tiles_lvgl.h` and `hatilvgl/tiles_lvgl.cpp`.
5. A focused `hatilvgl/tile_widget_<type>.cpp` renderer when the type has
   distinct behavior, plus `hatilvgl/component.yaml`.
6. Home Assistant REST service calls and WebSocket state handling when the tile
   reads or controls an entity.
7. Documentation and representative fixture data when the persisted contract
   intentionally changes.

Maintain backward-compatible reads for existing tile files. New fields need
explicit defaults when absent. Preserve unknown fields during browser
round-trips where current code does so. Validate grid bounds against the
runtime 7-column by 5-row layout, including span constraints and folder IDs.

Treat `hatiadmin/uitiles/` as legacy or auxiliary code unless call sites prove
it is active. Do not copy its older 4-by-4 model or tile enum into the active
7-by-5 implementation.

## Keep browser assets synchronized

Edit only the source files in `hatiadmin/assets/`. After changing
`admin.js` or `admin.css`, run from the repository root:

```powershell
Push-Location config\external_components\hatiadmin
node tools\generate-web-assets.mjs
node tools\generate-web-assets.mjs --check
Pop-Location
```

Commit the resulting changes in:

- `generated/admin_js_gzip.inc`
- `generated/admin_css_gzip.inc`
- `generated/web_admin_assets.h`
- the generated metadata block in `web_admin_assets.cpp`

Only files affected by the source asset need to change. A compile can invoke
the generator, but do not rely on that side effect as the only validation.

## Respect runtime constraints

- HATi runs on constrained ESP32 hardware. Avoid unbounded JSON documents,
  response buffers, duplicate full payloads, and large temporary allocations.
- Prefer PSRAM for large media or response buffers when existing code does.
- Keep LVGL object mutation on the ESPHome/LVGL loop task. WebSocket callbacks
  must queue state and let the loop apply UI updates.
- Never log Home Assistant tokens, authorization headers, session credentials,
  or secrets.
- Keep REST and WebSocket behavior consistent. REST seeds or controls state;
  WebSocket `state_changed` events keep visible tiles current.
- Preserve the existing API error behavior. Invalid input must return an
  explicit non-2xx response and useful JSON error rather than silently using a
  default.
- Stored tile grids belong in SPIFFS paths `/spiffs/t_f0.json` through
  `/spiffs/t_f9.json`; folder metadata uses `/spiffs/t_folders.json`.
- Keep the 128 KiB device request limit in mind for tile import/export.
- Do not weaken the existing authentication boundary for `/admin`.

## ESPHome and component wiring

For schema or codegen changes, keep `__init__.py`, C++ setters, YAML examples,
`AUTO_LOAD`, `component.yaml`, and IDF dependencies consistent.

Use `config/common/hati.yaml` for shared HATi wiring. Keep credentials in
`config/secrets.yaml` and reference them with `!secret`; never add credentials
to tracked YAML.

The primary compile target is:

```powershell
esphome config config\P4-10-sample2.yaml
esphome compile config\P4-10-sample2.yaml
```

Run `esphome config` for YAML or Python schema/codegen changes. Compile for C++,
component metadata, generated asset, dependency, or cross-component changes.
Do not erase flash, open device logs, or mutate device data
unless the user explicitly requests it. Upload firmware via `esphome upload config\P4-10-sample2.yaml --device OTA` automatically if compilation succeeds.

## Focused validation

Choose the smallest checks that cover the change:

- Web asset source or embedding:

  ```powershell
  Push-Location config\external_components\hatiadmin
  node tools\generate-web-assets.mjs --check
  Pop-Location
  ```

- Host-side Python syntax:

  ```powershell
  python -m py_compile config\external_components\hatiadmin\__init__.py
  python -m py_compile config\external_components\hatilvgl\__init__.py
  python -m py_compile config\external_components\hatidart\__init__.py
  python -m py_compile config\external_components\hatiserve\__init__.py
  ```

- Tile backup parser without touching a device:

  ```powershell
  python -c "import json, pathlib; p=pathlib.Path(r'config\external_components\hatiadmin\tools\backup\tiles-all.json'); d=json.loads(p.read_text(encoding='utf-8')); assert isinstance(d.get('grids'), dict)"
  ```

- Device tile round-trip, only with explicit approval and a supplied device
  URL:

  ```powershell
  python config\external_components\hatiadmin\tests\test_web_admin_local_tiles.py `
    --url http://DEVICE/admin/tiles `
    --file config\external_components\hatiadmin\tools\backup\tiles-all.json
  ```

  This test uploads tile data and is destructive to the selected folders.
  Back up the device first and never run it speculatively.

- Read-only device smoke checks, only when the user requests device
  verification:

  ```powershell
  curl.exe -I http://DEVICE/admin
  curl.exe http://DEVICE/admin/folders
  curl.exe "http://DEVICE/admin/tiles?folder=0"
  ```

When a full compile is impractical, report exactly which focused checks ran and
which hardware-dependent checks remain.

## Completion checklist

- Persisted numeric tile IDs and existing JSON remain compatible.
- Browser save/load/default behavior matches firmware parsing.
- The LVGL renderer handles the feature and safely receives live updates.
- Component metadata and ESPHome codegen include every new source/dependency.
- Generated web assets are current.
- Relevant docs or sample configuration are updated.
- Validation covers the actual changed layer; device-mutating checks were not
  run without approval.
