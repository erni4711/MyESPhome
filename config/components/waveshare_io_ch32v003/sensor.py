"""
Waveshare IO CH32V003 Voltage Sensor Component for ESPHome.

This component implements a voltage sensor for the Waveshare IO expansion board.
It acts as a PollingComponent to periodically read voltage and implements the
VoltageSampler interface.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, voltage_sampler
from esphome.const import (
    CONF_ID,
    CONF_REFERENCE_VOLTAGE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_VOLT,
)
from . import (
    CONF_WAVESHARE_IO_CH32V003_ID,
    WaveshareIOCH32V003Component,
    waveshare_io_ch32v003_ns,
)

# --- Class Definition ---
WaveshareIOCH32V003Sensor = waveshare_io_ch32v003_ns.class_(
    "WaveshareIOCH32V003Sensor",
    sensor.Sensor,
    cg.PollingComponent,
    voltage_sampler.VoltageSampler,
    cg.Parented.template(WaveshareIOCH32V003Component),
)

# --- Configuration Schema ---
CONFIG_SCHEMA = (
    sensor.sensor_schema(
        WaveshareIOCH32V003Sensor,
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    )
    .extend(
        {
            # Link to the main Hub component (Parent)
            cv.Required(CONF_WAVESHARE_IO_CH32V003_ID): cv.use_id(
                WaveshareIOCH32V003Component
            ),
            # Reference voltage for the voltage divider.
            cv.Optional(CONF_REFERENCE_VOLTAGE, default="9.9V"): cv.All(
                cv.voltage, 
                cv.Range(min=0.1)
            ),
        }
    )
    # Adds the polling interval configuration (default: 60s)
    .extend(cv.polling_component_schema("60s"))
)

async def to_code(config):
    """
    Code logic for the voltage sensor.
    """
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_parented(var, config[CONF_WAVESHARE_IO_CH32V003_ID])
    
    # Register as a component and sensor
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)

    # Set the configured reference voltage for calculation
    cg.add(var.set_reference_voltage(config[CONF_REFERENCE_VOLTAGE]))