import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

sd_card_ns = cg.esphome_ns.namespace("sd_card")
SdMmcCard = sd_card_ns.class_("SdMmcCard", cg.Component)

CONF_SD_MMC_ID = "sd_mmc_id"

CONF_CLK_PIN = "clk_pin"
CONF_CMD_PIN = "cmd_pin"
CONF_D0_PIN = "d0_pin"
CONF_FORMAT_ON_MOUNT_FAILURE = "format_on_mount_failure"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SdMmcCard),
    cv.Required(CONF_CLK_PIN): cv.int_,
    cv.Required(CONF_CMD_PIN): cv.int_,
    cv.Required(CONF_D0_PIN): cv.int_,
    cv.Optional(CONF_FORMAT_ON_MOUNT_FAILURE, default=False): cv.boolean,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_cmd_pin(config[CONF_CMD_PIN]))
    cg.add(var.set_d0_pin(config[CONF_D0_PIN]))
    cg.add(var.set_format_on_mount_failure(config[CONF_FORMAT_ON_MOUNT_FAILURE]))