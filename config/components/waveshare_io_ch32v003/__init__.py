"""
Waveshare IO CH32V003 Expansion Board Component for ESPHome.

This component provides support for the Waveshare IO expansion board based on the
CH32V003 microcontroller, communicating via I2C. It handles GPIO expansion
and voltage sampling features.
"""

from esphome import pins
import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INPUT,
    CONF_INVERTED,
    CONF_MODE,
    CONF_NUMBER,
    CONF_OUTPUT,
)

# --- Constants and Configuration Keys ---
CONF_WAVESHARE_IO_CH32V003_ID = "waveshare_io_ch32v003_id"
CONF_WAVESHARE_IO_CH32V003 = "waveshare_io_ch32v003"

CODEOWNERS = ["@fuzzybear62"]

# --- Component Loading and Dependencies ---
# Defines which other ESPHome components should be loaded automatically.
AUTO_LOAD = ["gpio_expander", "voltage_sampler"]

DEPENDENCIES = ["i2c"]
MULTI_CONF = True

# --- C++ Namespace and Class Definitions ---
waveshare_io_ch32v003_ns = cg.esphome_ns.namespace("waveshare_io_ch32v003")

WaveshareIOCH32V003Component = waveshare_io_ch32v003_ns.class_(
    "WaveshareIOCH32V003Component", cg.Component, i2c.I2CDevice
)
WaveshareIOCH32V003GPIOPin = waveshare_io_ch32v003_ns.class_(
    "WaveshareIOCH32V003GPIOPin",
    cg.GPIOPin,
    cg.Parented.template(WaveshareIOCH32V003Component),
)

# --- Main Configuration Schema ---
CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WaveshareIOCH32V003Component),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x24))  # Default I2C address 0x24
)

async def to_code(config):
    """
    Code generation logic for the main component.
    Registers the component and I2C device in the generated C++ code.
    """
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    # RIMOSSO: cg.add_library(...)
    # Non è necessario elencare i file sorgente locali. 
    # PlatformIO compilerà automaticamente tutti i .cpp nella cartella del componente.
    # Le guardie #ifdef nei file C++ gestiranno l'inclusione logica.

def validate_mode(value):
    """
    Validates the GPIO mode configuration.
    Ensures that a pin is configured as either Input OR Output, but not both simultaneously.
    """
    if not (value[CONF_INPUT] or value[CONF_OUTPUT]):
        raise cv.Invalid("Mode must be either input or output")
    if value[CONF_INPUT] and value[CONF_OUTPUT]:
        raise cv.Invalid("Mode must be either input or output")
    return value

# --- Pin Configuration Schema ---
# Defines how individual pins are configured in YAML.
# Validates pin numbers (0-7) and modes.
WAVESHARE_IO_PIN_SCHEMA = pins.gpio_base_schema(
    WaveshareIOCH32V003GPIOPin,
    cv.int_range(min=0, max=7),
    modes=[CONF_INPUT, CONF_OUTPUT],
    mode_validator=validate_mode,
    invertible=True,
).extend(
    {
        cv.Required(CONF_WAVESHARE_IO_CH32V003): cv.use_id(
            WaveshareIOCH32V003Component
        ),
    }
)

@pins.PIN_SCHEMA_REGISTRY.register(CONF_WAVESHARE_IO_CH32V003, WAVESHARE_IO_PIN_SCHEMA)
async def waveshare_io_pin_to_code(config):
    """
    Code generation logic for individual GPIO pins.
    Sets up the pin object, parent reference, pin number, inversion status, and IO flags.
    """
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_WAVESHARE_IO_CH32V003])
    cg.add(var.set_parent(parent))
    num = config[CONF_NUMBER]
    cg.add(var.set_pin(num))
    cg.add(var.set_inverted(config[CONF_INVERTED]))
    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))
    return var