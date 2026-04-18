#pragma once

#include "esphome/core/defines.h"

// Only compile this file if the 'sensor' component is used in YAML
#ifdef USE_SENSOR

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"
#include "waveshare_io_ch32v003.h"

namespace esphome {
namespace waveshare_io_ch32v003 {

/**
 * @brief Voltage Sensor component for the Waveshare IO CH32V003 expansion board.
 */
class WaveshareIOCH32V003Sensor : public sensor::Sensor,
                                  public PollingComponent,
                                  public voltage_sampler::VoltageSampler,
                                  public Parented<WaveshareIOCH32V003Component> {
 public:
  void set_reference_voltage(float reference_voltage) { this->reference_voltage_ = reference_voltage; }

  void setup() override;
  void update() override;
  void dump_config() override;
  
  float sample() override;

 protected:
  float reference_voltage_{3.3f}; 
};

}  // namespace waveshare_io_ch32v003
}  // namespace esphome

#endif // USE_SENSOR