import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import CONF_ID
from esphome.components import light, number, select


AUTO_LOAD = ["hatifonts", "select"]

CONF_HOME_ASSISTANT_URL = "home_assistant_url"
CONF_HOME_ASSISTANT_TOKEN = "home_assistant_token"
CONF_BACKLIGHT = "backlight"
CONF_SCREEN_TIMEOUT = "screen_timeout"
CONF_DISPLAY_FOLDER = "display_folder"

hatilvgl_ns = cg.esphome_ns.namespace("web_admin_local")
HATiLvglComponent = hatilvgl_ns.class_("HATiLvglComponent", cg.Component)
HATiFolderSelect = hatilvgl_ns.class_("HATiFolderSelect", select.Select)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HATiLvglComponent),
        cv.Optional(CONF_HOME_ASSISTANT_URL, default=""): cv.string_strict,
        cv.Optional(CONF_HOME_ASSISTANT_TOKEN, default=""): cv.string_strict,
        cv.Optional(CONF_BACKLIGHT): cv.use_id(light.LightState),
        cv.Optional(CONF_SCREEN_TIMEOUT): cv.use_id(number.Number),
        cv.Optional(
            CONF_DISPLAY_FOLDER,
            default={"name": "Displayed Folder"},
        ): select.select_schema(
            HATiFolderSelect,
            icon="mdi:folder",
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_home_assistant_url(config[CONF_HOME_ASSISTANT_URL]))
    cg.add(var.set_home_assistant_token(config[CONF_HOME_ASSISTANT_TOKEN]))
    if CONF_BACKLIGHT in config:
        cg.add(var.set_backlight(await cg.get_variable(config[CONF_BACKLIGHT])))
    if CONF_SCREEN_TIMEOUT in config:
        cg.add(var.set_screen_timeout(await cg.get_variable(config[CONF_SCREEN_TIMEOUT])))
    folder_select = await select.new_select(
        config[CONF_DISPLAY_FOLDER],
        options=[f"Folder {index}" for index in range(10)],
    )
    cg.add(var.set_folder_select(folder_select))
    add_idf_component(name="espressif/esp_websocket_client", ref="1.8.0")
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", True)
