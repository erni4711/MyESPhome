import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

CONF_URL_PREFIX_SD = "sd_url_prefix"
CONF_URL_PREFIX_SPIFFS = "spiffs_url_prefix"

hatiserve_ns = cg.esphome_ns.namespace("hatiserve")
HATiServe = hatiserve_ns.class_("HATiServe", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HATiServe),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Optional(CONF_URL_PREFIX_SD, default="sdcard"): cv.string_strict,
        cv.Optional(CONF_URL_PREFIX_SPIFFS, default="spiffs"): cv.string_strict,
    }
).extend(cv.COMPONENT_SCHEMA)

AUTO_LOAD = ["web_server_base", "spiffs"]


async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    cg.add(var.set_sd_prefix(config[CONF_URL_PREFIX_SD]))
    cg.add(var.set_spiffs_prefix(config[CONF_URL_PREFIX_SPIFFS]))
