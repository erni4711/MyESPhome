import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import sd_mmc_card

# Create a namespace for your component
my_ns = cg.esphome_ns.namespace("screenshot")

# Define the C++ class
Screenshot = my_ns.class_("Screenshot", cg.Component)

# Configuration schema
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Screenshot),
    cv.Optional(sd_mmc_card.CONF_SD_MMC_ID): cv.use_id(sd_mmc_card.SdMmcCard),
}).extend(cv.COMPONENT_SCHEMA)

# Code generation
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if sd_mmc_card.CONF_SD_MMC_ID in config:
        sdcard = await cg.get_variable(config[sd_mmc_card.CONF_SD_MMC_ID])
        cg.add(var.set_sd_mmc_card(sdcard))
