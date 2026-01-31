import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

# Import waveshare IO expander component types if present
try:
    from esphome.components import waveshare_io_ch32v003 as ch422g
except Exception:
    ch422g = None

sd_card_ns = cg.esphome_ns.namespace("sd_card")
SdMmcCard = sd_card_ns.class_("SdMmcCard", cg.Component)

CONF_SD_MMC_ID = "sd_mmc_id"

CONF_CLK_PIN = "clk_pin"
CONF_CMD_PIN = "cmd_pin"
CONF_D0_PIN = "d0_pin"
CONF_FORMAT_ON_MOUNT_FAILURE = "format_on_mount_failure"
CONF_CS_PIN = "cs_pin"
CONF_CH422G = "ch422g"
CONF_WAVESHARE_IO_CH32V003 = "waveshare_io_ch32v003"

# Schema for the optional cs_pin using the CH422G expander format used in this project
CS_PIN_SCHEMA = cv.All(
    cv.Schema({
        cv.Optional(CONF_CH422G): cv.use_id(ch422g.WaveshareIOCH32V003Component) if ch422g is not None else cv.declare_id(object),
        cv.Optional(CONF_WAVESHARE_IO_CH32V003): cv.use_id(ch422g.WaveshareIOCH32V003Component) if ch422g is not None else cv.declare_id(object),
        cv.Required('number'): cv.int_,
    }),
    cv.has_at_least_one_key(CONF_CH422G, CONF_WAVESHARE_IO_CH32V003),
)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SdMmcCard),
    cv.Optional(CONF_CLK_PIN): cv.int_,
    cv.Optional(CONF_CMD_PIN): cv.int_,
    cv.Optional(CONF_D0_PIN): cv.int_,
    cv.Optional(CONF_CS_PIN): CS_PIN_SCHEMA,
    cv.Optional(CONF_FORMAT_ON_MOUNT_FAILURE, default=False): cv.boolean,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    if CONF_CLK_PIN not in config or CONF_CMD_PIN not in config or CONF_D0_PIN not in config:
        return
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_cmd_pin(config[CONF_CMD_PIN]))
    cg.add(var.set_d0_pin(config[CONF_D0_PIN]))
    cg.add(var.set_format_on_mount_failure(config[CONF_FORMAT_ON_MOUNT_FAILURE]))

    if CONF_CS_PIN in config:
        cs_cfg = config[CONF_CS_PIN]
        expander_key = CONF_WAVESHARE_IO_CH32V003 if CONF_WAVESHARE_IO_CH32V003 in cs_cfg else CONF_CH422G
        expander = await cg.get_variable(cs_cfg[expander_key])
        cs_pin = cs_cfg['number']
        cg.add(var.set_cs_expander(expander, cs_pin))