import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID

CONF_URL_PREFIX = "url_prefix"

AUTO_LOAD = ["web_server_base", "web_server"]

web_admin_local_ns = cg.esphome_ns.namespace("web_admin_local")
WebAdminLocal = web_admin_local_ns.class_("WebAdminLocal", cg.Component)

schema = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebAdminLocal),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Optional(CONF_URL_PREFIX, default="admin"): cv.string_strict,
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
