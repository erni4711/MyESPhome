import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import include_builtin_idf_component


spiffs_ns = cg.esphome_ns.namespace("spiffs")
Spiffs = spiffs_ns.class_("Spiffs", cg.Component)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Spiffs),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    include_builtin_idf_component("spiffs")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
