import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CONF_STATE_FILE = "state_file"
CONF_SPEAKER_ID = "speaker_id"

hatidart_ns = cg.esphome_ns.namespace("hatidart")
HATiDart = hatidart_ns.class_("HATiDart", cg.Component)
speaker_ns = cg.esphome_ns.namespace("speaker")
Speaker = speaker_ns.class_("Speaker")


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HATiDart),
        cv.Optional(CONF_STATE_FILE, default="/spiffs/hatidart.json"): cv.string_strict,
        cv.Optional(CONF_SPEAKER_ID): cv.use_id(Speaker),
    }
).extend(cv.COMPONENT_SCHEMA)

AUTO_LOAD = ["lvgl", "spiffs", "hatifonts", "speaker"]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_state_file(config[CONF_STATE_FILE]))
    if CONF_SPEAKER_ID in config:
        cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER_ID])))
