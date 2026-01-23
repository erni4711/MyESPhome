#pragma once

#include "esphome.h"
#include <string>

namespace esphome {
namespace spi {
class SPIComponent;
}  // namespace spi
namespace waveshare_io_ch32v003 {
class WaveshareIOCH32V003Component;
}  // namespace waveshare_io_ch32v003
}  // namespace esphome

namespace esphome {
namespace sd_card {

class SdSpiCard : public Component {
 public:
  void setup() override;

  // Simple wrapper functions that delegate to the wave7sd helpers. This allows
  // other components that expect an "sd_card" type to work with the
  // existing wave7sd implementation.
  bool mount();
  void unmount();
  bool get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb);
  bool is_mounted();
  std::string list_dir_json(const std::string &path);
  bool read_file_to_string(const std::string &path, std::string &out);
  bool delete_file(const std::string &path);
  bool append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing = true);

  void set_format_on_mount_failure(bool v) { this->format_on_mount_failure_ = v; }
  void set_spi_parent(spi::SPIComponent *spi) { this->spi_ = spi; }
  void set_cs_expander(waveshare_io_ch32v003::WaveshareIOCH32V003Component *expander, uint8_t pin) {
    this->cs_expander_ = expander;
    this->cs_expander_pin_ = pin;
  }
  void set_gpio_cs_pin(uint8_t pin) { this->gpio_cs_pin_ = pin; }

 private:
  bool format_on_mount_failure_{false};
  spi::SPIComponent *spi_{};
  waveshare_io_ch32v003::WaveshareIOCH32V003Component *cs_expander_{};
  uint8_t cs_expander_pin_{255};
  uint8_t gpio_cs_pin_{255};
};

}  // namespace sd_card
}  // namespace esphome
