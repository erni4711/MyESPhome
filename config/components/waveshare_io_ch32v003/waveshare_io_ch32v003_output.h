#pragma once

#include "esphome/core/defines.h"

// Only compile this file if the 'output' component is used in YAML
#ifdef USE_OUTPUT 

#include "esphome/components/output/float_output.h"
#include "esphome/core/component.h"
#include "waveshare_io_ch32v003.h"

namespace esphome {
namespace waveshare_io_ch32v003 {

/**
 * @brief PWM Output component for the Waveshare IO CH32V003 expansion board.
 */
class WaveshareIOCH32V003Output : public output::FloatOutput, public Component, public Parented<WaveshareIOCH32V003Component> {
 public:
  void set_pwm_safe_range(uint8_t min_value, uint8_t max_value);
  void dump_config() override;

 protected:
  void write_state(float state) override;

  uint8_t pwm_min_value_{0};
  uint8_t pwm_max_value_{255};
};

}  // namespace waveshare_io_ch32v003
}  // namespace esphome

#endif // USE_OUTPUT