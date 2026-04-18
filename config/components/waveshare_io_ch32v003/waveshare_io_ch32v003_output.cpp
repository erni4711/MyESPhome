#include "waveshare_io_ch32v003_output.h"

// Only compile implementation if USE_OUTPUT is defined
#ifdef USE_OUTPUT

#include "esphome/core/log.h"
#include <algorithm> // For std::clamp, std::swap
#include <cmath>     // For roundf

namespace esphome {
namespace waveshare_io_ch32v003 {

static const char *const TAG = "waveshare_io_ch32v003.output";

void WaveshareIOCH32V003Output::set_pwm_safe_range(uint8_t min_value, uint8_t max_value) {
  if (min_value > max_value) {
    ESP_LOGW(TAG, "Invalid PWM safe range: min (%u) > max (%u). Swapping values.", min_value, max_value);
    std::swap(min_value, max_value);
  }
  this->pwm_min_value_ = min_value;
  this->pwm_max_value_ = max_value;
}

void WaveshareIOCH32V003Output::dump_config() {
  ESP_LOGCONFIG(TAG, "Waveshare IO PWM Output:");
  LOG_FLOAT_OUTPUT(this);
  ESP_LOGCONFIG(TAG, "  PWM Safe Range: [%u, %u]", this->pwm_min_value_, this->pwm_max_value_);
}

void WaveshareIOCH32V003Output::write_state(float state) {
  if (this->parent_->is_failed()) {
    return;
  }

  float safe_state = std::clamp(state, 0.0f, 1.0f);
  uint8_t pwm_value = static_cast<uint8_t>(roundf(safe_state * 255.0f));
  uint8_t final_pwm_value = std::clamp(pwm_value, this->pwm_min_value_, this->pwm_max_value_);

  ESP_LOGV(TAG, "PWM Write: state=%.3f -> raw=%u -> clamped=%u", safe_state, pwm_value, final_pwm_value);
  this->parent_->set_pwm_value(final_pwm_value);
}

}  // namespace waveshare_io_ch32v003
}  // namespace esphome

#endif // USE_OUTPUT