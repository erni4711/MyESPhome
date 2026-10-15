import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import i2c
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
)
from esphome.const import CONF_ID
from esphome.core.entity_helpers import setup_entity

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@erni4711"]
DEPENDENCIES = ["esp32", "i2c", "psram"]
AUTO_LOAD = ["camera"]

camera_ns = cg.esphome_ns.namespace("camera")
Camera = camera_ns.class_("Camera", cg.EntityBase, cg.Component)

p4_camera_ns = cg.esphome_ns.namespace("p4_camera")
P4Camera = p4_camera_ns.class_("P4Camera", Camera, i2c.I2CDevice)

CameraResolution = p4_camera_ns.enum("CameraResolution")

CONF_CAMERA_MODEL = "camera_model"
CONF_RESOLUTION = "resolution"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_MAX_FRAMERATE = "max_framerate"
CONF_VERTICAL_FLIP = "vertical_flip"
CONF_HORIZONTAL_MIRROR = "horizontal_mirror"
CONF_ROTATION = "rotation"
CONF_RESET_PIN = "reset_pin"
CONF_POWER_DOWN_PIN = "power_down_pin"
CONF_SCCB_FREQUENCY = "sccb_frequency"
CONF_STANDBY_TIMEOUT = "standby_timeout"
CONF_WARMUP_FRAMES = "warmup_frames"

# Deprecated options from earlier versions of this component.
CONF_FRAMERATE = "framerate"
CONF_FLIP_MIRROR = "flip_mirror"
CONF_PIXEL_FORMAT = "pixel_format"
CONF_EXTERNAL_CLOCK_PIN = "external_clock_pin"
CONF_FREQUENCY = "frequency"
CONF_ADDRESS_SENSOR_OV5647 = "address_sensor_ov5647"
IGNORED_OPTIONS = (
    CONF_PIXEL_FORMAT,
    CONF_EXTERNAL_CLOCK_PIN,
    CONF_FREQUENCY,
    CONF_ADDRESS_SENSOR_OV5647,
)

# OV5647 MIPI-CSI modes provided by esp_cam_sensor 2.x:
# name -> (C++ enum, Kconfig format suffix)
RESOLUTIONS = {
    "800X640": (CameraResolution.RESOLUTION_VGA, "RAW8_800X640_50FPS"),
    "800X800": (CameraResolution.RESOLUTION_SVGA, "RAW8_800X800_50FPS"),
    "800X1280": (CameraResolution.RESOLUTION_800X1280, "RAW8_800X1280_50FPS"),
    "1280X960": (CameraResolution.RESOLUTION_1280X960, "RAW10_1280X960_BINNING_45FPS"),
    "1920X1080": (CameraResolution.RESOLUTION_FHD, "RAW10_1920X1080_30FPS"),
}
RESOLUTION_ALIASES = {"VGA": "800X640", "SVGA": "800X800", "FHD": "1920X1080"}


def validate_resolution(value):
    value = cv.string_strict(value).upper().replace(" ", "")
    value = RESOLUTION_ALIASES.get(value, value)
    if value not in RESOLUTIONS:
        raise cv.Invalid(
            f"Unknown resolution '{value}', must be one of: "
            f"{', '.join(list(RESOLUTIONS) + list(RESOLUTION_ALIASES))}"
        )
    return value


def handle_deprecated_options(config):
    for key in IGNORED_OPTIONS:
        if key in config:
            _LOGGER.warning(
                "p4_camera: '%s' has no effect and will be removed; please delete it", key
            )
    if CONF_FRAMERATE in config:
        _LOGGER.warning("p4_camera: 'framerate' is deprecated, use 'max_framerate'")
        if CONF_MAX_FRAMERATE not in config:
            config[CONF_MAX_FRAMERATE] = float(config[CONF_FRAMERATE])
    if config.get(CONF_FLIP_MIRROR):
        _LOGGER.warning(
            "p4_camera: 'flip_mirror' is deprecated, use 'vertical_flip' and 'horizontal_mirror'"
        )
        config.setdefault(CONF_VERTICAL_FLIP, True)
        config.setdefault(CONF_HORIZONTAL_MIRROR, True)
    return config


CONFIG_SCHEMA = cv.All(
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(P4Camera),
            cv.Optional(CONF_CAMERA_MODEL, default="OV5647"): cv.one_of(
                "OV5647", upper=True
            ),
            cv.Optional(CONF_RESOLUTION, default="800X1280"): validate_resolution,
            cv.Optional(CONF_JPEG_QUALITY, default=80): cv.int_range(min=1, max=100),
            cv.Optional(CONF_MAX_FRAMERATE): cv.float_range(min=0.1, max=30.0),
            cv.Optional(CONF_VERTICAL_FLIP): cv.boolean,
            cv.Optional(CONF_HORIZONTAL_MIRROR): cv.boolean,
            # Counter-clockwise, applied by the PPA before JPEG encoding.
            cv.Optional(CONF_ROTATION, default=0): cv.one_of(0, 90, 180, 270, int=True),
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_POWER_DOWN_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_SCCB_FREQUENCY, default="400kHz"): cv.All(
                cv.frequency, cv.int_range(min=10000, max=1000000)
            ),
            cv.Optional(
                CONF_STANDBY_TIMEOUT, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_WARMUP_FRAMES, default=15): cv.int_range(min=0, max=100),
            # Deprecated
            cv.Optional(CONF_FRAMERATE): cv.int_range(min=1, max=60),
            cv.Optional(CONF_FLIP_MIRROR): cv.boolean,
            cv.Optional(CONF_PIXEL_FORMAT): cv.string,
            cv.Optional(CONF_EXTERNAL_CLOCK_PIN): cv.Any(
                cv.int_, pins.internal_gpio_output_pin_schema
            ),
            cv.Optional(CONF_FREQUENCY): cv.frequency,
            cv.Optional(CONF_ADDRESS_SENSOR_OV5647): cv.i2c_address,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x36)),
    cv.only_with_framework("esp-idf"),
    only_on_variant(supported=[VARIANT_ESP32P4], msg_prefix="p4_camera"),
    handle_deprecated_options,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add_define("USE_CAMERA")
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    resolution_enum, format_suffix = RESOLUTIONS[config[CONF_RESOLUTION]]
    cg.add(var.set_resolution(resolution_enum))
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    cg.add(var.set_max_framerate(config.get(CONF_MAX_FRAMERATE, 10.0)))
    cg.add(var.set_vertical_flip(config.get(CONF_VERTICAL_FLIP, False)))
    cg.add(var.set_horizontal_mirror(config.get(CONF_HORIZONTAL_MIRROR, False)))
    cg.add(var.set_rotation(config[CONF_ROTATION]))
    cg.add(var.set_sccb_frequency(config[CONF_SCCB_FREQUENCY]))
    cg.add(var.set_standby_timeout(config[CONF_STANDBY_TIMEOUT].total_milliseconds))
    cg.add(var.set_warmup_frames(config[CONF_WARMUP_FRAMES]))
    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    if CONF_POWER_DOWN_PIN in config:
        cg.add(var.set_power_down_pin(config[CONF_POWER_DOWN_PIN]))

    add_idf_component(name="espressif/esp_video", ref="~2.0")

    # Sensor driver (esp_cam_sensor 2.x Kconfig names).
    add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647", True)
    add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647_AUTO_DETECT_MIPI_INTERFACE_SENSOR", True)
    add_idf_sdkconfig_option(f"CONFIG_CAMERA_OV5647_MIPI_{format_suffix}", True)
    add_idf_sdkconfig_option(f"CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_{format_suffix}", True)
    add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647_CSI_LINESYNC_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647_DEFAULT_IPA_JSON_CONFIGURATION_FILE", True)

    # MIPI-CSI capture through the ISP (RAW -> RGB565) with auto exposure / white balance.
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP", True)
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER", True)

    # The prebuilt libesp_ipa.a contains ESP_LOGD calls (e.g. "esp_ipa_ian: AE: luma_avg=...")
    # for every frame. ESPHome disables runtime log filtering, which makes those calls
    # always print. Re-enable it so CONFIG_LOG_DEFAULT_LEVEL applies to precompiled libs.
    add_idf_sdkconfig_option("CONFIG_LOG_DYNAMIC_LEVEL_CONTROL", True)
    add_idf_sdkconfig_option("CONFIG_LOG_TAG_LEVEL_IMPL_NONE", False)
    add_idf_sdkconfig_option("CONFIG_LOG_TAG_LEVEL_IMPL_LINKED_LIST", True)
