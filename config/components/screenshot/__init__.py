import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import sd_mmc_card
from esphome.components import tab5_camera
try:
    from .. import sd_spi_card
except Exception:
    sd_spi_card = None

# Create a namespace for your component
my_ns = cg.esphome_ns.namespace("screenshot")

# Define the C++ class
Screenshot = my_ns.class_("Screenshot", cg.Component)

CONF_CAMERA_ID = "camera_id"

SD_MMC_ID_KEY = getattr(
    sd_mmc_card,
    "CONF_SD_MMC_ID",
    getattr(sd_mmc_card, "CONF_SD_MMC_CARD_ID", "sd_mmc_id"),
)
SD_MMC_CLASS = getattr(
    sd_mmc_card,
    "SdMmc",
    getattr(sd_mmc_card, "SdMmcCard", cg.Component),
)

# Configuration schema
schema = cv.Schema({
    cv.GenerateID(): cv.declare_id(Screenshot),
    cv.Optional(SD_MMC_ID_KEY): cv.use_id(SD_MMC_CLASS),
    cv.Optional(CONF_CAMERA_ID): cv.use_id(tab5_camera.Tab5Camera),
})
if sd_spi_card is not None:
    schema = schema.extend({
        cv.Optional(sd_spi_card.CONF_SD_SPI_ID): cv.use_id(sd_spi_card.SdSpiCard),
    })

CONFIG_SCHEMA = schema.extend(cv.COMPONENT_SCHEMA)

# Code generation
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if SD_MMC_ID_KEY in config:
        sdcard = await cg.get_variable(config[SD_MMC_ID_KEY])
        cg.add(var.set_sd_mmc_card(sdcard))
    if CONF_CAMERA_ID in config:
        camera = await cg.get_variable(config[CONF_CAMERA_ID])
        cg.add(var.set_camera(camera))
        cg.add_build_flag("-DHAVE_CAMERA=1")
    if sd_spi_card is not None and sd_spi_card.CONF_SD_SPI_ID in config:
        sdspi = await cg.get_variable(config[sd_spi_card.CONF_SD_SPI_ID])
        cg.add(var.set_sd_spi_card(sdspi))
