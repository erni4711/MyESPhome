#pragma once
#include <string>
#include <vector>

// ── Home Assistant websocket client ───────────────────────────────────────
//
// Maintains a single ws(s):// connection to Home Assistant's
// `/api/websocket` API, authenticates with a long-lived access token,
// subscribes to `state_changed` events and (best effort) requests the
// current state of every configured entity on connect.
//
// All network I/O and JSON parsing happens on the esp_websocket_client
// library's own FreeRTOS task (the event handler callback). Matching
// entity updates are queued and only ever applied to LVGL widgets from
// ha_ws_client_loop(), which must be called from the ESPHome loop() task.

namespace web_admin_local {

// Stores the Home Assistant base URL + long-lived access token used for the
// websocket connection. Call once, before ha_ws_client_start(). Mirrors
// set_home_assistant_credentials() (used for the REST switch toggle path)
// declared in tiles_lvgl.h. Never logs the token.
void ha_ws_client_configure(const std::string &home_assistant_url,
                             const std::string &home_assistant_token);

// Starts the websocket client (idempotent; a no-op once already started).
// Does nothing if the Home Assistant URL/token have not been configured.
// Safe to call before Wi-Fi is connected: the client auto-reconnects.
void ha_ws_client_start();

// Replaces the set of entity ids the client cares about. `state_changed`
// events (and get_states results) for any other entity id are dropped
// before they are queued for the loop task. Called whenever the configured
// tile grids change (see TilesLvglRenderer::refresh_folder()).
void ha_ws_client_set_entity_filter(const std::vector<std::string> &entity_ids);

// Drains any Home Assistant state updates queued by the websocket task and
// applies them to registered LVGL widgets (see register_ha_entity_widget()
// in tiles_lvgl.h). MUST be called only from the ESPHome loop() task.
void ha_ws_client_loop();

}  // namespace web_admin_local
