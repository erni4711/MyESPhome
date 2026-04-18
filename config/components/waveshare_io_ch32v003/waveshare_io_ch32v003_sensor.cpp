#include "waveshare_io_ch32v003_sensor.h"

// Only compile implementation if USE_SENSOR is defined
#ifdef USE_SENSOR

#include "esphome/core/log.h"
#include <algorithm> // For std::min
#include <cmath>     // For NAN, std::isnan

namespace esphome {
namespace waveshare_io_ch32v003 {

static const char *const TAG = "waveshare_io_ch32v003.sensor";

void WaveshareIOCH32V003Sensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Waveshare IO ADC Sensor...");
}

void WaveshareIOCH32V003Sensor::dump_config() {
  LOG_SENSOR("", "Waveshare IO ADC Sensor", this);
  ESP_LOGCONFIG(TAG, "  Reference Voltage: %.2fV", this->reference_voltage_);
  LOG_UPDATE_INTERVAL(this);
}

float WaveshareIOCH32V003Sensor::sample() {
  if (this->parent_->is_failed()) {
    return NAN;
  }

  uint16_t raw_value = this->parent_->get_adc_value();
  raw_value = std::min<uint16_t>(raw_value, 1023);

  float voltage = (float(raw_value) / 1023.0f) * this->reference_voltage_;

  ESP_LOGV(TAG, "ADC Sample: Raw=%u -> Voltage=%.3f V", raw_value, voltage);
  return voltage;
}

void WaveshareIOCH32V003Sensor::update() {
  float voltage = this->sample();

  if (!std::isnan(voltage)) {
    this->publish_state(voltage);
  }
}

}  // namespace waveshare_io_ch32v003
}  // namespace esphome

#endif // USE_SENSOR