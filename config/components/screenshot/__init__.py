import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import sd_spi_card

# Create a namespace for your component
my_ns = cg.esphome_ns.namespace("screenshot")

# Define the C++ class
Screenshot = my_ns.class_("Screenshot", cg.Component)

# Configuration schema
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Screenshot),
    cv.Optional(sd_spi_card.CONF_SPI_ID): cv.use_id(sd_spi_card.SdSpiCard),
}).extend(cv.COMPONENT_SCHEMA)

# Code generation
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if sd_spi_card.CONF_SPI_ID in config:
        sdcard = await cg.get_variable(config[sd_spi_card.CONF_SPI_ID])
        cg.add(var.set_sd_spi_card(sdcard))
