import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

from esphome.components import web_server_base, web_server
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from .. import sd_mmc_card
try:
    from .. import sd_spi_card
except Exception:
    sd_spi_card = None

CONF_URL_PREFIX = "url_prefix"
CONF_ROOT_PATH = "root_path"
CONF_ENABLE_DELETION = "enable_deletion"
CONF_ENABLE_DOWNLOAD = "enable_download"
CONF_ENABLE_UPLOAD = "enable_upload"

# Accept either the base web server or the regular web_server (some configs
# use one or the other). Allow either sd_mmc_card or sd_spi_card when present.
AUTO_LOAD = ["web_server_base", "web_server", "sd_mmc_card"]
if sd_spi_card is not None:
    AUTO_LOAD.append("sd_spi_card")

sd_file_server_ns = cg.esphome_ns.namespace("sd_file_server")
SDFileServer = sd_file_server_ns.class_("SDFileServer", cg.Component)

schema = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SDFileServer),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server.WebServer),
        cv.Optional(sd_mmc_card.CONF_SD_MMC_ID): cv.use_id(sd_mmc_card.SdMmcCard),
        cv.Optional(CONF_URL_PREFIX, default="file"): cv.string_strict,
        cv.Optional(CONF_ROOT_PATH, default="/"): cv.string_strict,
        cv.Optional(CONF_ENABLE_DELETION, default=False): cv.boolean,
        cv.Optional(CONF_ENABLE_DOWNLOAD, default=False): cv.boolean,
        cv.Optional(CONF_ENABLE_UPLOAD, default=False): cv.boolean,
    }
)
if sd_spi_card is not None:
    schema = schema.extend(
        {cv.Optional(sd_spi_card.CONF_SD_SPI_ID): cv.use_id(sd_spi_card.SdSpiCard)}
    )

CONFIG_SCHEMA = cv.All(
    cv.require_esphome_version(2025,7,0),
    schema.extend(cv.COMPONENT_SCHEMA),
    cv.has_exactly_one_key(
        sd_mmc_card.CONF_SD_MMC_ID,
        sd_spi_card.CONF_SD_SPI_ID if sd_spi_card is not None else sd_mmc_card.CONF_SD_MMC_ID,
    ),
)

async def to_code(config):
    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], paren)
    await cg.register_component(var, config)
    if sd_mmc_card.CONF_SD_MMC_ID in config:
        sdmmc = await cg.get_variable(config[sd_mmc_card.CONF_SD_MMC_ID])
        cg.add(var.set_sd_mmc_card(sdmmc))
    if sd_spi_card is not None and sd_spi_card.CONF_SD_SPI_ID in config:
        sdspi = await cg.get_variable(config[sd_spi_card.CONF_SD_SPI_ID])
        cg.add(var.set_sd_spi_card(sdspi))
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    cg.add(var.set_root_path(config[CONF_ROOT_PATH]))
    cg.add(var.set_deletion_enabled(config[CONF_ENABLE_DELETION]))
    cg.add(var.set_download_enabled(config[CONF_ENABLE_DOWNLOAD]))
    cg.add(var.set_upload_enabled(config[CONF_ENABLE_UPLOAD]))
    cg.add_define("USE_SD_CARD_WEBSERVER")
