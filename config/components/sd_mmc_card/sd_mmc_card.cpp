#include "sd_mmc_card.h"
#include <cstddef>
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#if defined(CONFIG_SDMMC_ENABLE_POWER_CONTROL) && __has_include("sd_pwr_ctrl_by_on_chip_ldo.h")
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#define ESPHOME_SDMMC_HAS_PWR_CTRL 1
#else
#define ESPHOME_SDMMC_HAS_PWR_CTRL 0
#endif
#ifdef INIT_SD_AFTER_WIFI_INIT
#include "esphome/components/wifi/wifi_component.h"
#endif
#include <cerrno>
#include <cstring>
#include "esp_err.h"
#include <cstdio>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>

extern "C" {
typedef struct {
  bool format_if_mount_failed;
  int max_files;
  size_t allocation_unit_size;
  bool disk_status_check_enable;
  bool use_one_fat;
} esp_vfs_fat_mount_config_t;

typedef esp_vfs_fat_mount_config_t esp_vfs_fat_sdmmc_mount_config_t;

esp_err_t esp_vfs_fat_sdmmc_mount(const char *base_path, const sdmmc_host_t *host_config, const void *slot_config,
                                  const esp_vfs_fat_sdmmc_mount_config_t *mount_config, sdmmc_card_t **out_card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card);
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *out_total_bytes, uint64_t *out_free_bytes);
}

namespace esphome {
namespace sd_card {

static const char *TAG = "sd_mmc_card";
static const char *MOUNT_POINT = "/sdcard";

static std::string to_vfs_path(const std::string &path) {
  if (path.empty()) return std::string(MOUNT_POINT);
  if (path.rfind(MOUNT_POINT, 0) == 0) return path;
  if (path[0] == '/') return std::string(MOUNT_POINT) + path;
  return std::string(MOUNT_POINT) + "/" + path;
}

void SdMmcCard::setup() {
#ifdef INIT_SD_AFTER_WIFI_INIT
  if (wifi::global_wifi_component != nullptr) {
    if (wifi::global_wifi_component->is_connected()) {
      this->mount_at_ms_ = millis() + 10000;
      this->mount_scheduled_ = true;
      ESP_LOGI(TAG, "SDMMC mount scheduled 10s after WiFi is up");
      return;
    }
    this->waiting_for_wifi_ = true;
    ESP_LOGI(TAG, "Waiting for WiFi before SDMMC mount");
    return;
  }
#endif

  bool ok = this->mount();
  if (ok) {
    ESP_LOGI(TAG, "SD card mounted");
  } else {
    ESP_LOGW(TAG, "SD card mount failed");
  }
}

void SdMmcCard::loop() {
  if (this->mounted_) return;

#ifdef INIT_SD_AFTER_WIFI_INIT
  if (this->waiting_for_wifi_) {
    if (wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected()) {
      this->waiting_for_wifi_ = false;
      this->mount_at_ms_ = millis() + 10000;
      this->mount_scheduled_ = true;
      ESP_LOGI(TAG, "WiFi is up; SDMMC mount scheduled in 10s");
    }
  }
#endif

  if (this->mount_scheduled_) {
    const uint32_t now = millis();
    if ((int32_t) (now - this->mount_at_ms_) >= 0) {
      this->mount_scheduled_ = false;
      bool ok = this->mount();
      if (ok) {
        ESP_LOGI(TAG, "SD card mounted");
      } else {
        ESP_LOGW(TAG, "SD card mount failed");
      }
    }
  }
}

bool SdMmcCard::mount() {
  if (this->mounted_) return true;

  if (this->cs_pin_ != nullptr) {
    this->cs_pin_->setup();
    this->cs_pin_->pin_mode(esphome::gpio::FLAG_OUTPUT);
    // Match the demo: keep CS/power deasserted (high) when idle.
    this->cs_pin_->digital_write(true);
    delay(50);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  esp_err_t err = ESP_OK;
#if ESPHOME_SDMMC_HAS_PWR_CTRL
  sd_pwr_ctrl_ldo_config_t ldo_config = {
      .ldo_chan_id = 4,
  };
  sd_pwr_ctrl_handle_t pwr_ctrl_handle = nullptr;
  err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
    return false;
  }
  host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif
  
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  if (this->mode_1bit_) {
    host.flags = SDMMC_HOST_FLAG_1BIT;
    slot_config.width = 1;
  } else {
    host.flags = SDMMC_HOST_FLAG_4BIT;
    slot_config.width = 4;
  }
  ESP_LOGI(TAG, "SDMMC bus width: %ubit", this->mode_1bit_ ? 1U : 4U);
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->d0_pin_);
  slot_config.d1 = this->mode_1bit_ ? GPIO_NUM_NC : static_cast<gpio_num_t>(this->d1_pin_);
  slot_config.d2 = this->mode_1bit_ ? GPIO_NUM_NC : static_cast<gpio_num_t>(this->d2_pin_);
  slot_config.d3 = this->mode_1bit_ ? GPIO_NUM_NC : static_cast<gpio_num_t>(this->d3_pin_);
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  // Ensure pull-ups are applied on CMD/D0 for reliable card init
  gpio_set_pull_mode(static_cast<gpio_num_t>(this->cmd_pin_), GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(this->d0_pin_), GPIO_PULLUP_ONLY);
  if (!this->mode_1bit_) {
    gpio_set_pull_mode(static_cast<gpio_num_t>(this->d1_pin_), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(this->d2_pin_), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(this->d3_pin_), GPIO_PULLUP_ONLY);
  }
  gpio_set_pull_mode(static_cast<gpio_num_t>(this->clk_pin_), GPIO_FLOATING);

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = this->format_on_mount_failure_,
      .max_files = 5,
      .allocation_unit_size = 64 * 1024};

  err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->mounted_card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SDMMC mount failed: %s", esp_err_to_name(err));
    this->mounted_card_ = nullptr;
    return false;
  }

  this->mounted_ = true;
  return true;
}

void SdMmcCard::unmount() {
  if (this->mounted_) {
    if (this->mounted_card_ != nullptr) {
      esp_vfs_fat_sdcard_unmount(MOUNT_POINT, this->mounted_card_);
      this->mounted_card_ = nullptr;
    }
    this->mounted_ = false;
  }
  if (this->cs_pin_ != nullptr) {
    this->cs_pin_->digital_write(true);
  }
}

bool SdMmcCard::get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb) {
  uint64_t total = 0;
  uint64_t free = 0;
  if (esp_vfs_fat_info(MOUNT_POINT, &total, &free) != ESP_OK) return false;
  *total_kb = total / 1024ULL;
  *available_kb = free / 1024ULL;
  return true;
}

bool SdMmcCard::is_mounted() { return this->mounted_; }

std::string SdMmcCard::list_dir_json(const std::string &path) {
  std::string out = "[";
  std::string p = path;
  if (p.empty()) p = MOUNT_POINT;
  std::string vfs_path = to_vfs_path(p);
  DIR *dir = opendir(vfs_path.c_str());
  if (dir == nullptr) return "[]";
  bool first = true;
  for (dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
    if (entry->d_name[0] == 0) break;
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    if (!first) out += ",";
    first = false;
    std::string name = entry->d_name;
    std::string full_path = vfs_path + "/" + name;
    struct stat info;
    bool is_dir = false;
    unsigned long size = 0;
    if (stat(full_path.c_str(), &info) == 0) {
      is_dir = S_ISDIR(info.st_mode);
      if (!is_dir) size = static_cast<unsigned long>(info.st_size);
    }
    out += "{\"name\":\"" + name + "\",";
    out += "\"size\":" + std::to_string(size) + ",";
    out += "\"is_dir\":" + std::string(is_dir ? "true" : "false") + ",\"mtime\":0}";
  }
  closedir(dir);
  out += "]";
  return out;
}

bool SdMmcCard::stat_file(const std::string &path, size_t *size, bool *is_dir) {
  std::string vfs_path = to_vfs_path(path);
  struct stat info;
  if (stat(vfs_path.c_str(), &info) != 0) return false;
  if (size != nullptr) *size = static_cast<size_t>(info.st_size);
  if (is_dir != nullptr) *is_dir = S_ISDIR(info.st_mode);
  return true;
}

bool SdMmcCard::read_file_to_string(const std::string &path, std::string &out) {
  std::string vfs_path = to_vfs_path(path);
  struct stat info;
  if (stat(vfs_path.c_str(), &info) != 0) return false;
  FILE *file = fopen(vfs_path.c_str(), "rb");
  if (file == nullptr) return false;
  out.clear();
  if (info.st_size > 0) {
    out.resize((size_t) info.st_size);
    size_t offset = 0;
    const size_t chunk_size = 4096;
    while (offset < out.size()) {
      size_t to_read = out.size() - offset;
      if (to_read > chunk_size) to_read = chunk_size;
      size_t br = fread(&out[offset], 1, to_read, file);
      if (br != to_read) {
        fclose(file);
        return false;
      }
      offset += br;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  fclose(file);
  return true;
}

bool SdMmcCard::read_file_to_buffer(const std::string &path, uint8_t **out_buf, size_t *out_size) {
  if (out_buf == nullptr || out_size == nullptr) return false;
  *out_buf = nullptr;
  *out_size = 0;
  std::string vfs_path = to_vfs_path(path);
  struct stat info;
  if (stat(vfs_path.c_str(), &info) != 0) return false;
  if (info.st_size == 0) return false;

  FILE *file = fopen(vfs_path.c_str(), "rb");
  if (file == nullptr) return false;

  size_t total = static_cast<size_t>(info.st_size);
  uint8_t *buf = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr) {
    buf = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_8BIT);
  }
  if (buf == nullptr) {
    fclose(file);
    return false;
  }

  size_t offset = 0;
  const size_t chunk_size = 4096;
  while (offset < total) {
    size_t to_read = total - offset;
    if (to_read > chunk_size) to_read = chunk_size;
    size_t br = fread(buf + offset, 1, to_read, file);
    if (br != to_read) {
      fclose(file);
      heap_caps_free(buf);
      return false;
    }
    offset += br;
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  fclose(file);
  *out_buf = buf;
  *out_size = total;
  return true;
}

bool SdMmcCard::stream_file(const std::string &path, const std::function<bool(const uint8_t *, size_t)> &on_chunk,
                            size_t chunk_size) {
  if (chunk_size == 0) return false;
  std::string vfs_path = to_vfs_path(path);
  FILE *file = fopen(vfs_path.c_str(), "rb");
  if (file == nullptr) return false;
  uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    ESP_LOGW(TAG, "stream_file buffer alloc failed (%u bytes)", (unsigned) chunk_size);
    fclose(file);
    return false;
  }
  bool ok = true;
  for (;;) {
    size_t br = fread(buffer, 1, chunk_size, file);
    if (br == 0) break;
    if (!on_chunk(buffer, (size_t) br)) {
      ok = false;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  fclose(file);
  heap_caps_free(buffer);
  return ok;
}

bool SdMmcCard::delete_file(const std::string &path) {
  std::string vfs_path = to_vfs_path(path);
  if (remove(vfs_path.c_str()) == 0) return true;
  ESP_LOGW(TAG, "delete_file failed for %s: %d", vfs_path.c_str(), errno);
  return false;
}

bool SdMmcCard::append_file(const char *path, const uint8_t *data, size_t len) {
  if (path == nullptr || data == nullptr) return false;
  return this->append_file_chunk(path, data, len, true);
}

bool SdMmcCard::append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing) {
  std::string vfs_path = to_vfs_path(path);
  const char *mode = create_if_missing ? "ab" : "rb+";
  FILE *file = fopen(vfs_path.c_str(), mode);
  if (file == nullptr) {
    ESP_LOGW(TAG, "open failed for %s: %d", vfs_path.c_str(), errno);
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    ESP_LOGW(TAG, "seek failed for %s: %d", vfs_path.c_str(), errno);
    fclose(file);
    return false;
  }
  bool ok = true;
  size_t offset = 0;
  while (offset < len) {
    size_t to_write = std::min<size_t>(len - offset, 512);
    size_t bw = fwrite(data + offset, 1, to_write, file);
    if (bw != to_write) {
      ESP_LOGW(TAG, "write failed for %s: %d (wrote %u of %u)", vfs_path.c_str(), errno, (unsigned) bw,
               (unsigned) to_write);
      ok = false;
      break;
    }
    offset += bw;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  fclose(file);
  return ok;
}

bool SdMmcCard::begin_write(const std::string &path) {
  if (this->write_file_open_) {
    fclose(this->write_file_);
    this->write_file_open_ = false;
    this->write_path_.clear();
  }
  std::string vfs_path = to_vfs_path(path);
  this->write_file_ = fopen(vfs_path.c_str(), "wb");
  if (this->write_file_ == nullptr) {
    ESP_LOGW(TAG, "open failed for %s: %d", vfs_path.c_str(), errno);
    return false;
  }
  this->write_file_open_ = true;
  this->write_path_ = vfs_path;
  return true;
}

bool SdMmcCard::write_chunk(const uint8_t *data, size_t len) {
  if (!this->write_file_open_) {
    ESP_LOGW(TAG, "write_chunk called without open file");
    return false;
  }
  size_t bw = fwrite(data, 1, len, this->write_file_);
  if (bw != len) {
    ESP_LOGW(TAG, "write failed for %s: %d (wrote %u of %u)", this->write_path_.c_str(), errno,
             (unsigned) bw, (unsigned) len);
    return false;
  }
  return true;
}

bool SdMmcCard::end_write() {
  if (!this->write_file_open_) return true;
  fclose(this->write_file_);
  this->write_file_ = nullptr;
  this->write_file_open_ = false;
  this->write_path_.clear();
  return true;
}

}  // namespace sd_card
}  // namespace esphome