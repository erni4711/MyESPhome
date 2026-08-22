import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, camera
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_FREQUENCY,
)
from esphome import pins

CODEOWNERS = ["@erni4711"]
DEPENDENCIES = ["i2c", "esp32"]

p4_camera_ns = cg.esphome_ns.namespace('p4_camera')
P4Camera = p4_camera_ns.class_('P4Camera', cg.Component, i2c.I2CDevice)

CONF_RESOLUTION = 'resolution'
CONF_PIXEL_FORMAT = 'pixel_format'
CONF_JPEG_QUALITY = 'jpeg_quality'
CONF_FRAMERATE = 'framerate'
CONF_EXTERNAL_CLOCK_PIN = 'external_clock_pin'
CONF_RESET_PIN = 'reset_pin'
CONF_ADDRESS_SENSOR_OV5647 = 'address_sensor_ov5647'
CONF_CAMERA_MODEL = 'camera_model'
CONF_FLIP_MIRROR = 'flip_mirror'

CameraResolution = p4_camera_ns.enum('CameraResolution')
RESOLUTION_SVGA = CameraResolution.RESOLUTION_SVGA
RESOLUTION_VGA = CameraResolution.RESOLUTION_VGA
RESOLUTION_FHD = CameraResolution.RESOLUTION_FHD

PixelFormat = p4_camera_ns.enum('PixelFormat')
PIXEL_FORMAT_RGB565 = PixelFormat.PIXEL_FORMAT_RGB565
PIXEL_FORMAT_YUV422 = PixelFormat.PIXEL_FORMAT_YUV422
PIXEL_FORMAT_RAW8 = PixelFormat.PIXEL_FORMAT_RAW8
PIXEL_FORMAT_JPEG = PixelFormat.PIXEL_FORMAT_JPEG

CAMERA_RESOLUTIONS = {
    'SVGA': RESOLUTION_SVGA,
    'VGA': RESOLUTION_VGA,
    'FHD': RESOLUTION_FHD,
}

PIXEL_FORMATS = {
    'RGB565': PIXEL_FORMAT_RGB565,
    'YUV422': PIXEL_FORMAT_YUV422,
    'RAW8': PIXEL_FORMAT_RAW8,
    'JPEG': PIXEL_FORMAT_JPEG,
}

# Supported camera sensor models (maps to IDF CONFIG_CAMERA_<MODEL>)
CAMERA_MODELS = {
    'OV5647': 'OV5647',
    'OV5640': 'OV5640',
    'OV2640': 'OV2640',
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(P4Camera),
            cv.Optional(CONF_NAME, default="P4 Camera"): cv.string,
            cv.Optional(CONF_EXTERNAL_CLOCK_PIN, default=36): cv.Any(
                cv.int_range(min=0, max=50),
                pins.internal_gpio_output_pin_schema
            ),
            cv.Optional(CONF_FREQUENCY, default=24000000): cv.int_range(min=6000000, max=40000000),
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_ADDRESS_SENSOR_OV5647, default=0x36): cv.i2c_address,
            cv.Optional(CONF_CAMERA_MODEL, default='OV5647'): cv.enum(CAMERA_MODELS, upper=True),
            cv.Optional(CONF_RESOLUTION, default='SVGA'): cv.enum(CAMERA_RESOLUTIONS, upper=True),
            cv.Optional(CONF_PIXEL_FORMAT, default='RGB565'): cv.enum(PIXEL_FORMATS, upper=True),
            cv.Optional(CONF_JPEG_QUALITY, default=10): cv.int_range(min=1, max=63),
            cv.Optional(CONF_FRAMERATE, default=30): cv.int_range(min=1, max=60),
            cv.Optional(CONF_FLIP_MIRROR, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x36))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_name(config.get(CONF_NAME)))

    # External clock pin
    ext_clock_pin_config = config[CONF_EXTERNAL_CLOCK_PIN]
    if isinstance(ext_clock_pin_config, int):
        cg.add(var.set_external_clock_pin(ext_clock_pin_config))
    else:
        pin_num = ext_clock_pin_config[pins.CONF_NUMBER]
        cg.add(var.set_external_clock_pin(pin_num))

    cg.add(var.set_external_clock_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_sensor_address(config[CONF_ADDRESS_SENSOR_OV5647]))

    cg.add(var.set_resolution(config[CONF_RESOLUTION]))
    cg.add(var.set_pixel_format(config[CONF_PIXEL_FORMAT]))
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    cg.add(var.set_framerate(config[CONF_FRAMERATE]))
    cg.add(var.set_flip_mirror(config[CONF_FLIP_MIRROR]))
    # Camera model -> add build flag / sdkconfig as required by esp32-camera
    camera_model = config.get(CONF_CAMERA_MODEL, 'OV5647')
    if camera_model == 'OV5647':
        cg.add_build_flag("-DCONFIG_CAMERA_OV5647=1")
        add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647", True)
    elif camera_model == 'OV5640':
        cg.add_build_flag("-DCONFIG_CAMERA_OV5640=1")
        add_idf_sdkconfig_option("CONFIG_CAMERA_OV5640", True)
    elif camera_model == 'OV2640':
        cg.add_build_flag("-DCONFIG_CAMERA_OV2640=1")
        add_idf_sdkconfig_option("CONFIG_CAMERA_OV2640", True)

    if CONF_RESET_PIN in config:
        reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
        cg.add(var.set_reset_pin(reset_pin))

    # build flags
    cg.add_build_flag("-DBOARD_HAS_PSRAM")
    cg.add_build_flag("-DCONFIG_CAMERA_CORE0=1")
    cg.add_build_flag("-DCONFIG_CAMERA_OV5647=1")
    cg.add_build_flag("-DUSE_ESP_CAMERA")
    cg.add_build_flag("-DUSE_ESP32_VARIANT_ESP32P4")
    # Ensure the ESP-IDF esp32-camera component is pulled in for esp_camera.h
    add_idf_component(name="espressif/esp32-camera", ref="2.1.1")
    add_idf_sdkconfig_option("CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW", True)
    add_idf_sdkconfig_option("CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY", False)
    # Enable ISP and P4-specific ISP driver support
    cg.add_build_flag("-DUSE_ESP32P4_ISP_CAMERA")
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_DVP_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_DEMOSAIC_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_LSC_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_WBG_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_SHARPEN_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_CCM_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_SOC_ISP_BF_SUPPORTED", True)
    # The ISP driver is provided by the ESP-IDF framework components;
    # enabling the SDKCONFIG options (above) ensures the headers are available.
