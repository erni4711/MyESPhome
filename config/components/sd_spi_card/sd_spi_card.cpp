#include "sd_spi_card.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "ff.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/waveshare_io_ch32v003/waveshare_io_ch32v003.h"
#include "esphome/core/hal.h"
#include <cstdio>
#include <sys/stat.h>

// Forward-declare the minimal wave7sd API used by this shim so we don't
// depend on a specific header include path in generated TUs.
namespace wave7sd {
bool mount();
void unmount();
bool get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb);
bool is_mounted();
std::string list_dir_json(const std::string &path);
bool read_file_to_string(const std::string &path, std::string &out);
bool delete_file(const std::string &path);
bool append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing = true);
void set_spi_component(esphome::spi::SPIComponent *spi);
void set_cs_expander(esphome::waveshare_io_ch32v003::WaveshareIOCH32V003Component *expander, uint8_t pin);
void set_gpio_cs_pin(uint8_t pin);
void set_format_on_mount_failure(bool format_on_mount_failure);
}

#include "esphome/core/log.h"

namespace esphome {
namespace sd_card {

static const char *TAG = "sd_spi_card";

void SdSpiCard::setup() {
  wave7sd::set_spi_component(this->spi_);
  wave7sd::set_cs_expander(this->cs_expander_, this->cs_expander_pin_);
  wave7sd::set_gpio_cs_pin(this->gpio_cs_pin_);
  wave7sd::set_format_on_mount_failure(this->format_on_mount_failure_);
  bool ok = wave7sd::mount();
  if (ok) {
    ESP_LOGI(TAG, "SD card mounted");
  } else {
    ESP_LOGW(TAG, "SD card mount failed");
  }
}

bool SdSpiCard::mount() {
  ESP_LOGD(TAG, "SdSpiCard::mount() -> calling wave7sd::mount()");
  wave7sd::set_spi_component(this->spi_);
  wave7sd::set_cs_expander(this->cs_expander_, this->cs_expander_pin_);
  wave7sd::set_gpio_cs_pin(this->gpio_cs_pin_);
  wave7sd::set_format_on_mount_failure(this->format_on_mount_failure_);
  return wave7sd::mount();
}

void SdSpiCard::unmount() {
  ESP_LOGD(TAG, "SdSpiCard::unmount() -> calling wave7sd::unmount()");
  wave7sd::unmount();
}

bool SdSpiCard::get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb) {
  return wave7sd::get_capacity_kb(total_kb, available_kb);
}

bool SdSpiCard::is_mounted() { return wave7sd::is_mounted(); }

std::string SdSpiCard::list_dir_json(const std::string &path) { return wave7sd::list_dir_json(path); }

bool SdSpiCard::read_file_to_string(const std::string &path, std::string &out) { return wave7sd::read_file_to_string(path, out); }

bool SdSpiCard::delete_file(const std::string &path) { return wave7sd::delete_file(path); }

bool SdSpiCard::append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing) {
  return wave7sd::append_file_chunk(path, data, len, create_if_missing);
}

}  // namespace sd_card
}  // namespace esphome

// If the platform doesn't compile a separate `wave7sd` TU, provide a
// local implementation here so the shim links correctly. This mirrors
// the helpers used by the SdSpiCard shim.
namespace wave7sd {

static const char *MOUNT_POINT = "/sdcard";
static esphome::spi::SPIComponent *spi_component = nullptr;
static esphome::waveshare_io_ch32v003::WaveshareIOCH32V003Component *cs_expander = nullptr;
static uint8_t cs_expander_pin = 255;
static uint8_t gpio_cs_pin = 255;
static bool format_on_mount_failure = false;
static bool mounted = false;
static sdmmc_card_t *mounted_card = nullptr;
static sdmmc_host_t sdspi_host;
static esp_err_t (*orig_do_transaction)(int slot, sdmmc_command_t *cmd) = nullptr;

static esp_err_t expander_do_transaction(int slot, sdmmc_command_t *cmd) {
  if (cs_expander != nullptr && cs_expander_pin != 255) {
    cs_expander->digital_write(cs_expander_pin, false);
  }
  esp_err_t err = orig_do_transaction != nullptr ? orig_do_transaction(slot, cmd) : ESP_FAIL;
  if (cs_expander != nullptr && cs_expander_pin != 255) {
    cs_expander->digital_write(cs_expander_pin, true);
  }
  return err;
}

class SPIComponentAccessor : public esphome::spi::SPIComponent {
 public:
  ::SPIInterface get_interface() const { return this->interface_; }
  bool using_hw() const { return this->using_hw_; }
  esphome::GPIOPin *get_clk_pin() const { return this->clk_pin_; }
  esphome::GPIOPin *get_mosi_pin() const { return this->sdo_pin_; }
  esphome::GPIOPin *get_miso_pin() const { return this->sdi_pin_; }
  const std::vector<uint8_t> &get_data_pins() const { return this->data_pins_; }
};

void set_spi_component(esphome::spi::SPIComponent *spi) { spi_component = spi; }

void set_cs_expander(esphome::waveshare_io_ch32v003::WaveshareIOCH32V003Component *expander, uint8_t pin) {
  cs_expander = expander;
  cs_expander_pin = pin;
}

void set_gpio_cs_pin(uint8_t pin) { gpio_cs_pin = pin; }

void set_format_on_mount_failure(bool value) { format_on_mount_failure = value; }

bool mount() {
  if (mounted) return true;
  if (spi_component == nullptr) {
    return false;
  }

  auto *spi = static_cast<SPIComponentAccessor *>(spi_component);
  if (!spi->using_hw()) {
    return false;
  }

  if (cs_expander != nullptr && cs_expander_pin != 255) {
    cs_expander->pin_mode(cs_expander_pin, esphome::gpio::FLAG_OUTPUT);
    // Match the demo: keep CS deasserted (high) when idle.
    cs_expander->digital_write(cs_expander_pin, true);
  } else {
    return false;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = format_on_mount_failure,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  sdspi_host = SDSPI_HOST_DEFAULT();
  sdspi_host.slot = spi->get_interface();
  orig_do_transaction = sdspi_host.do_transaction;
  sdspi_host.do_transaction = expander_do_transaction;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.host_id = spi->get_interface();
  if (gpio_cs_pin == 255) {
    ESP_LOGE("sd_spi_card", "gpio_cs_pin not set; SDSPI requires a real GPIO CS pin");
    return false;
  }
  slot_config.gpio_cs = static_cast<gpio_num_t>(gpio_cs_pin);

  esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &sdspi_host, &slot_config, &mount_config, &mounted_card);
  if (err != ESP_OK) {
    mounted_card = nullptr;
    return false;
  }
  mounted = true;
  return true;
}

void unmount() {
  if (mounted) {
    if (mounted_card != nullptr) {
      esp_vfs_fat_sdcard_unmount(MOUNT_POINT, mounted_card);
      mounted_card = nullptr;
    }
    mounted = false;
  }
  if (cs_expander != nullptr && cs_expander_pin != 255) {
    cs_expander->digital_write(cs_expander_pin, true);
  }
}

bool get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb) {
  // Leave existing FATFS-based implementation if available; fall back to false.
  FATFS *fs;
  DWORD fre_clust, fre_sect, tot_sect;
  FRESULT res = f_getfree(MOUNT_POINT + 1, &fre_clust, &fs);
  if (res != FR_OK) return false;
  tot_sect = (fs->n_fatent - 2) * fs->csize;
  fre_sect = fre_clust * fs->csize;
  uint64_t sector_size = 512ULL;
  *total_kb = (tot_sect * sector_size) / 1024ULL;
  *available_kb = (fre_sect * sector_size) / 1024ULL;
  return true;
}

std::string list_dir_json(const std::string &path) {
  std::string out = "[";
  std::string p = path;
  if (p.empty()) p = "/";
  FF_DIR dir;
  FILINFO fno;
  FRESULT res = f_opendir(&dir, p.c_str());
  if (res != FR_OK) return "[]";
  bool first = true;
  for (;;) {
    res = f_readdir(&dir, &fno);
    if (res != FR_OK || fno.fname[0] == 0) break;
    if (!first) out += ",";
    first = false;
    std::string name = fno.fname;
    unsigned long size = fno.fsize;
    bool is_dir = (fno.fattrib & AM_DIR) != 0;
    out += "{\"name\":\"" + name + "\",";
    out += "\"size\":" + std::to_string(size) + ",";
    out += "\"is_dir\":" + std::string(is_dir ? "true" : "false") + ",\"mtime\":0}";
  }
  f_closedir(&dir);
  out += "]";
  return out;
}

bool read_file_to_string(const std::string &path, std::string &out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return false; }
  rewind(f);
  out.clear();
  if (sz > 0) {
    out.resize((size_t)sz);
    if (fread(&out[0], 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
  }
  fclose(f);
  return true;
}

bool delete_file(const std::string &path) { return std::remove(path.c_str()) == 0; }

bool append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing) {
  const char *mode = create_if_missing ? "ab" : "ab";
  FILE *f = fopen(path.c_str(), mode);
  if (!f) return false;
  size_t written = fwrite(data, 1, len, f);
  fclose(f);
  return written == len;
}

bool is_mounted() { return mounted; }

}  // namespace wave7sd
