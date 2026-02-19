from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import include_builtin_idf_component

sd_card_ns = cg.esphome_ns.namespace("sd_card")
SdMmcCard = sd_card_ns.class_("SdMmcCard", cg.Component)

CONF_SD_MMC_ID = "sd_mmc_id"

CONF_CLK_PIN = "clk_pin"
CONF_CMD_PIN = "cmd_pin"
CONF_D0_PIN = "d0_pin"
CONF_D1_PIN = "d1_pin"
CONF_D2_PIN = "d2_pin"
CONF_D3_PIN = "d3_pin"
CONF_MODE_1BIT = "mode_1bit"
CONF_FORMAT_ON_MOUNT_FAILURE = "format_on_mount_failure"
CONF_CS_PIN = "cs_pin"


def _validate_bus_width(config):
    has_4bit_pins = any(
        key in config for key in (CONF_D1_PIN, CONF_D2_PIN, CONF_D3_PIN)
    )
    if has_4bit_pins:
        missing = [
            name
            for name in (CONF_D1_PIN, CONF_D2_PIN, CONF_D3_PIN)
            if name not in config
        ]
        if missing:
            raise cv.Invalid("d1_pin, d2_pin, and d3_pin must all be set together")

    mode_1bit = config.get(CONF_MODE_1BIT)
    if mode_1bit is False and not has_4bit_pins:
        raise cv.Invalid("d1_pin, d2_pin, and d3_pin are required when mode_1bit is false")

    return config

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(SdMmcCard),
        cv.Required(CONF_CLK_PIN): cv.int_,
        cv.Required(CONF_CMD_PIN): cv.int_,
        cv.Required(CONF_D0_PIN): cv.int_,
        cv.Optional(CONF_D1_PIN): cv.int_,
        cv.Optional(CONF_D2_PIN): cv.int_,
        cv.Optional(CONF_D3_PIN): cv.int_,
        cv.Optional(CONF_MODE_1BIT): cv.boolean,
        cv.Optional(CONF_CS_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_FORMAT_ON_MOUNT_FAILURE, default=False): cv.boolean,
    }).extend(cv.COMPONENT_SCHEMA),
    _validate_bus_width,
)


async def to_code(config):
    include_builtin_idf_component("fatfs")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_cmd_pin(config[CONF_CMD_PIN]))
    cg.add(var.set_d0_pin(config[CONF_D0_PIN]))
    mode_1bit = config.get(CONF_MODE_1BIT)
    if mode_1bit is None:
        mode_1bit = not any(
            key in config for key in (CONF_D1_PIN, CONF_D2_PIN, CONF_D3_PIN)
        )
    cg.add(var.set_mode_1bit(mode_1bit))
    if not mode_1bit:
        cg.add(var.set_d1_pin(config[CONF_D1_PIN]))
        cg.add(var.set_d2_pin(config[CONF_D2_PIN]))
        cg.add(var.set_d3_pin(config[CONF_D3_PIN]))
    cg.add(var.set_format_on_mount_failure(config[CONF_FORMAT_ON_MOUNT_FAILURE]))

    if CONF_CS_PIN in config:
        cs_pin = await cg.gpio_pin_expression(config[CONF_CS_PIN])
        cg.add(var.set_cs_pin(cs_pin))