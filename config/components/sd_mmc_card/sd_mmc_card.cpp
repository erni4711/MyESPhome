#include "sd_mmc_card.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "ff.h"
#include "driver/gpio.h"
#include "esphome/core/log.h"
#include <cerrno>
#include <cstring>
#include "esp_err.h"
#include <cstdio>
#include <sys/stat.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>

namespace esphome {
namespace sd_card {

static const char *TAG = "sd_mmc_card";
static const char *MOUNT_POINT = "/sdcard";

static std::string to_fatfs_path(const std::string &path) {
  if (path.empty()) return "0:/";
  if (path.rfind(MOUNT_POINT, 0) == 0) {
    std::string rest = path.substr(std::strlen(MOUNT_POINT));
    if (rest.empty()) return "0:/";
    if (rest[0] != '/') rest = "/" + rest;
    return "0:" + rest;
  }
  if (path[0] == '/') return "0:" + path;
  return "0:/" + path;
}

void SdMmcCard::setup() {
  bool ok = this->mount();
  if (ok) {
    ESP_LOGI(TAG, "SD card mounted");
  } else {
    ESP_LOGW(TAG, "SD card mount failed");
  }
}

bool SdMmcCard::mount() {
  if (this->mounted_) return true;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->d0_pin_);
  slot_config.d1 = GPIO_NUM_NC;
  slot_config.d2 = GPIO_NUM_NC;
  slot_config.d3 = GPIO_NUM_NC;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = this->format_on_mount_failure_,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->mounted_card_);
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
}

bool SdMmcCard::get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb) {
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

bool SdMmcCard::is_mounted() { return this->mounted_; }

std::string SdMmcCard::list_dir_json(const std::string &path) {
  std::string out = "[";
  std::string p = path;
  if (p.empty()) p = MOUNT_POINT;
  std::string fatfs_path = to_fatfs_path(p);
  FF_DIR dir;
  FILINFO fno;
  FRESULT res = f_opendir(&dir, fatfs_path.c_str());
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

bool SdMmcCard::stat_file(const std::string &path, size_t *size, bool *is_dir) {
  std::string fatfs_path = to_fatfs_path(path);
  FILINFO info;
  FRESULT res = f_stat(fatfs_path.c_str(), &info);
  if (res != FR_OK) return false;
  if (size != nullptr) *size = (size_t) info.fsize;
  if (is_dir != nullptr) *is_dir = (info.fattrib & AM_DIR) != 0;
  return true;
}

bool SdMmcCard::read_file_to_string(const std::string &path, std::string &out) {
  std::string fatfs_path = to_fatfs_path(path);
  FILINFO info;
  FRESULT res = f_stat(fatfs_path.c_str(), &info);
  if (res != FR_OK) return false;
  FIL file;
  res = f_open(&file, fatfs_path.c_str(), FA_READ);
  if (res != FR_OK) return false;
  out.clear();
  if (info.fsize > 0) {
    out.resize((size_t) info.fsize);
    size_t offset = 0;
    const size_t chunk_size = 4096;
    while (offset < out.size()) {
      UINT br = 0;
      size_t to_read = out.size() - offset;
      if (to_read > chunk_size) to_read = chunk_size;
      res = f_read(&file, &out[offset], (UINT) to_read, &br);
      if (res != FR_OK || br != to_read) {
        f_close(&file);
        return false;
      }
      offset += br;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  f_close(&file);
  return true;
}

bool SdMmcCard::read_file_to_buffer(const std::string &path, uint8_t **out_buf, size_t *out_size) {
  if (out_buf == nullptr || out_size == nullptr) return false;
  *out_buf = nullptr;
  *out_size = 0;
  std::string fatfs_path = to_fatfs_path(path);
  FILINFO info;
  FRESULT res = f_stat(fatfs_path.c_str(), &info);
  if (res != FR_OK) return false;
  if (info.fsize == 0) return false;

  FIL file;
  res = f_open(&file, fatfs_path.c_str(), FA_READ);
  if (res != FR_OK) return false;

  size_t total = (size_t) info.fsize;
  uint8_t *buf = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr) {
    buf = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_8BIT);
  }
  if (buf == nullptr) {
    f_close(&file);
    return false;
  }

  size_t offset = 0;
  const size_t chunk_size = 4096;
  while (offset < total) {
    UINT br = 0;
    size_t to_read = total - offset;
    if (to_read > chunk_size) to_read = chunk_size;
    res = f_read(&file, buf + offset, (UINT) to_read, &br);
    if (res != FR_OK || br != to_read) {
      f_close(&file);
      heap_caps_free(buf);
      return false;
    }
    offset += br;
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  f_close(&file);
  *out_buf = buf;
  *out_size = total;
  return true;
}

bool SdMmcCard::stream_file(const std::string &path, const std::function<bool(const uint8_t *, size_t)> &on_chunk,
                            size_t chunk_size) {
  if (chunk_size == 0) return false;
  std::string fatfs_path = to_fatfs_path(path);
  FIL file;
  FRESULT res = f_open(&file, fatfs_path.c_str(), FA_READ);
  if (res != FR_OK) return false;
  std::vector<uint8_t> buffer;
  buffer.resize(chunk_size);
  bool ok = true;
  for (;;) {
    UINT br = 0;
    res = f_read(&file, buffer.data(), (UINT) buffer.size(), &br);
    if (res != FR_OK) {
      ok = false;
      break;
    }
    if (br == 0) break;
    if (!on_chunk(buffer.data(), (size_t) br)) {
      ok = false;
      break;
    }
  }
  f_close(&file);
  return ok;
}

bool SdMmcCard::delete_file(const std::string &path) {
  std::string fatfs_path = to_fatfs_path(path);
  FRESULT res = f_unlink(fatfs_path.c_str());
  if (res == FR_OK) return true;
  ESP_LOGW(TAG, "delete_file failed for %s: %d", fatfs_path.c_str(), (int) res);
  return false;
}

bool SdMmcCard::append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing) {
  std::string fatfs_path = to_fatfs_path(path);
  FIL file;
  BYTE mode = FA_WRITE;
  if (create_if_missing) mode |= FA_OPEN_ALWAYS;
  FRESULT res = f_open(&file, fatfs_path.c_str(), mode);
  if (res != FR_OK) {
    ESP_LOGW(TAG, "open failed for %s: %d", fatfs_path.c_str(), (int) res);
    return false;
  }
  res = f_lseek(&file, f_size(&file));
  if (res != FR_OK) {
    ESP_LOGW(TAG, "seek failed for %s: %d", fatfs_path.c_str(), (int) res);
    f_close(&file);
    return false;
  }
  UINT bw = 0;
  res = f_write(&file, data, (UINT) len, &bw);
  if (res != FR_OK || bw != len) {
    ESP_LOGW(TAG, "write failed for %s: %d (wrote %u of %u)", fatfs_path.c_str(), (int) res, (unsigned) bw,
             (unsigned) len);
  }
  f_close(&file);
  return res == FR_OK && bw == len;
}

}  // namespace sd_card
}  // namespace esphome