import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import shutil
from pathlib import Path
import subprocess

CONF_URL_PREFIX = "url_prefix"
CONF_HOME_ASSISTANT_URL = "home_assistant_url"
CONF_HOME_ASSISTANT_TOKEN = "home_assistant_token"

AUTO_LOAD = ["web_server_base", "web_server", "http_request"]

web_admin_local_ns = cg.esphome_ns.namespace("web_admin_local")
WebAdminLocal = web_admin_local_ns.class_("WebAdminLocal", cg.Component)

schema = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebAdminLocal),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Optional(CONF_URL_PREFIX, default="admin"): cv.string_strict,
        cv.Optional(CONF_HOME_ASSISTANT_URL, default=""): cv.string_strict,
        cv.Optional(CONF_HOME_ASSISTANT_TOKEN, default=""): cv.string_strict,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.require_esphome_version(2025, 7, 0),
    schema.extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], paren)
    await cg.register_component(var, config)
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    cg.add(var.set_home_assistant_url(config[CONF_HOME_ASSISTANT_URL]))
    cg.add(var.set_home_assistant_token(config[CONF_HOME_ASSISTANT_TOKEN]))

    # Home Assistant WebSocket API client (ha_ws_client.cpp) for live
    # sensor/energy entity state updates. Managed by ESP-IDF's component
    # manager, matching the p4_camera component's espressif/esp32-camera
    # convention.
    add_idf_component(name="espressif/esp_websocket_client", ref="1.8.0")
    # Local Home Assistant installs commonly serve wss:// with a
    # self-signed certificate; allow esp-tls to skip full certificate
    # verification instead of refusing to connect. See ha_ws_client.cpp.
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", True)

    # Ensure generated assets exist (run Node generator if available), then
    # copy files from this component's `generated/` folder into the ESPHome
    # build tree so C++ includes like "generated/web_admin_assets.h" and
    # "generated/admin_js_gzip.inc" resolve at compile time.
    try:
        comp_dir = Path(__file__).parent
        gen_dir = comp_dir / 'generated'
        gen_script = comp_dir / 'tools' / 'generate-web-assets.mjs'
        build_path = Path(CORE.build_path)
        component_build_dir = build_path / 'src' / 'esphome' / 'components' / 'web_admin_local'
        component_build_dir.mkdir(parents=True, exist_ok=True)

        # ui_fonts.h is kept with the generated font sources but is included
        # from the component root by all LVGL renderers.
        ui_fonts_src = comp_dir / 'fonts' / 'ui_fonts.h'
        if ui_fonts_src.exists():
            shutil.copy2(ui_fonts_src, component_build_dir / 'ui_fonts.h')

        # Run the Node.js generator to keep generated files up to date (non-fatal).
        if gen_script.exists():
            node_bin = shutil.which('node')
            if node_bin:
                try:
                    subprocess.run([node_bin, str(gen_script)], cwd=str(comp_dir), check=True)
                    print(f"[web_admin_local] ran asset generator: {gen_script}")
                except Exception as e:
                    print(f"[web_admin_local] asset generator failed: {e}")
            else:
                print("[web_admin_local] node not found on PATH; skipping auto-generation")

        if not gen_dir.exists():
            return

        # Use CORE.build_path which ESPHome sets to the active build directory
        # (e.g. <config_dir>/.esphome/build/<nodename>).
        dest_dir = build_path / 'src' / 'esphome' / 'components' / 'web_admin_local' / 'generated'
        dest_dir.mkdir(parents=True, exist_ok=True)

        copied = 0
        for f in gen_dir.iterdir():
            if f.is_file():
                shutil.copy2(f, dest_dir / f.name)
                copied += 1
        if copied:
            print(f"[web_admin_local] copied {copied} generated file(s) to {dest_dir}")

        # Copy tile_types/ subdirectory (per-type LVGL widget implementations).
        # ESPHome only syncs the component root; subdirectories must be copied manually.
        tile_types_src = comp_dir / 'tile_types'
        if tile_types_src.is_dir():
            tile_types_dst = build_path / 'src' / 'esphome' / 'components' / 'web_admin_local' / 'tile_types'
            tile_types_dst.mkdir(parents=True, exist_ok=True)
            tt_copied = 0
            for f in tile_types_src.iterdir():
                if f.is_file() and f.suffix in ('.cpp', '.h'):
                    shutil.copy2(f, tile_types_dst / f.name)
                    tt_copied += 1
            if tt_copied:
                print(f"[web_admin_local] copied {tt_copied} tile_types file(s) to {tile_types_dst}")

        # Also copy tiles_lvgl.h and tiles_lvgl.cpp to the build root.
        for fname in ('tiles_lvgl.h', 'tiles_lvgl.cpp',
                      'web_admin_lvgl_fonts.h',
                      'mdi_icons.h', 'mdi_icons.cpp'):
            src_file = comp_dir / fname
            if src_file.exists():
                dst_file = build_path / 'src' / 'esphome' / 'components' / 'web_admin_local' / fname
                shutil.copy2(src_file, dst_file)

        # ESPHome does not synchronize nested component directories
        # automatically. Copy the complete fonts directory so all generated
        # font sources and headers remain available in the build tree.
        font_dir = comp_dir / 'fonts'
        fonts_dst = component_build_dir / 'fonts'
        if font_dir.is_dir():
            shutil.copytree(font_dir, fonts_dst, dirs_exist_ok=True)
            for font_file in font_dir.iterdir():
                if font_file.is_file():
                    shutil.copy2(font_file, component_build_dir / font_file.name)
            print(f"[web_admin_local] copied fonts directory to {fonts_dst}")
    except Exception as e:
        # Non-fatal: log and continue
        print(f"[web_admin_local] failed to copy generated files: {e}")
