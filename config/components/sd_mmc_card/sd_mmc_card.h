#pragma once

#include "esphome.h"
#include "sdmmc_cmd.h"
#include <string>

namespace esphome {
namespace sd_card {

class SdMmcCard : public Component {
 public:
  void setup() override;

  bool mount();
  void unmount();
  bool get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb);
  bool is_mounted();
  std::string list_dir_json(const std::string &path);
  bool read_file_to_string(const std::string &path, std::string &out);
  bool delete_file(const std::string &path);
  bool append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing = true);

  void set_format_on_mount_failure(bool v) { this->format_on_mount_failure_ = v; }
  void set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }
  void set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }
  void set_d0_pin(uint8_t pin) { this->d0_pin_ = pin; }

 protected:
  bool format_on_mount_failure_{false};
  uint8_t clk_pin_{255};
  uint8_t cmd_pin_{255};
  uint8_t d0_pin_{255};
  bool mounted_{false};
  sdmmc_card_t *mounted_card_{nullptr};
};

}  // namespace sd_card
}  // namespace esphome