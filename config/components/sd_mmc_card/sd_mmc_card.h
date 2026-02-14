#pragma once

#include "esphome.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include <functional>
#include <string>

namespace esphome {
class GPIOPin;
}  // namespace esphome

namespace esphome {
namespace sd_card {

class SdMmcCard : public Component {
 public:
  void setup() override;
  void loop() override;

  bool mount();
  void unmount();
  bool get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb);
  bool is_mounted();
  std::string list_dir_json(const std::string &path);
  bool stat_file(const std::string &path, size_t *size, bool *is_dir);
  bool read_file_to_string(const std::string &path, std::string &out);
  bool read_file_to_buffer(const std::string &path, uint8_t **out_buf, size_t *out_size);
  bool stream_file(const std::string &path, const std::function<bool(const uint8_t *, size_t)> &on_chunk,
                   size_t chunk_size = 4096);
  bool delete_file(const std::string &path);
  bool append_file(const char *path, const uint8_t *data, size_t len);
  bool append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing = true);
  bool begin_write(const std::string &path);
  bool write_chunk(const uint8_t *data, size_t len);
  bool end_write();

  void set_format_on_mount_failure(bool v) { this->format_on_mount_failure_ = v; }
  void set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }
  void set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }
  void set_d0_pin(uint8_t pin) { this->d0_pin_ = pin; }
  void set_d1_pin(uint8_t pin) { this->d1_pin_ = pin; }
  void set_d2_pin(uint8_t pin) { this->d2_pin_ = pin; }
  void set_d3_pin(uint8_t pin) { this->d3_pin_ = pin; }
  void set_mode_1bit(bool v) { this->mode_1bit_ = v; }
  void set_cs_pin(GPIOPin *pin) { this->cs_pin_ = pin; }

 protected:
  bool waiting_for_wifi_{false};
  bool mount_scheduled_{false};
  uint32_t mount_at_ms_{0};
  bool format_on_mount_failure_{false};
  uint8_t clk_pin_{255};
  uint8_t cmd_pin_{255};
  uint8_t d0_pin_{255};
  uint8_t d1_pin_{255};
  uint8_t d2_pin_{255};
  uint8_t d3_pin_{255};
  bool mode_1bit_{true};
  GPIOPin *cs_pin_{nullptr};
  bool mounted_{false};
  sdmmc_card_t *mounted_card_{nullptr};
  FIL write_file_{};
  bool write_file_open_{false};
  std::string write_path_{};
};

}  // namespace sd_card
}  // namespace esphome