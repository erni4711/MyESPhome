#include "spiffs.h"

#include <esp_partition.h>
#include <esp_spiffs.h>
#include <esp_task_wdt.h>

#include "esphome/core/log.h"

namespace esphome {
namespace spiffs {

static const char *const TAG = "spiffs";
static constexpr char BASE_PATH[] = "/spiffs";

static const esp_partition_t *find_partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

bool is_mounted() {
  const esp_partition_t *partition = find_partition();
  return partition != nullptr && esp_spiffs_mounted(partition->label);
}

bool ensure_mounted() {
  if (is_mounted()) {
    return true;
  }

  const esp_partition_t *partition = find_partition();
  if (partition == nullptr) {
    ESP_LOGW(TAG, "No SPIFFS data partition found");
    return false;
  }

  esp_vfs_spiffs_conf_t config = {};
  config.base_path = BASE_PATH;
  config.partition_label = partition->label;
  config.max_files = 16;
  config.format_if_mount_failed = true;

  // Formatting can take long enough to trip the application watchdog.
  esp_task_wdt_delete(nullptr);
  const esp_err_t result = esp_vfs_spiffs_register(&config);
  const esp_err_t wdt_result = esp_task_wdt_add(nullptr);
  if (wdt_result != ESP_OK && wdt_result != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "Failed to re-enable task watchdog: %s",
             esp_err_to_name(wdt_result));
  }

  if (result != ESP_OK) {
    ESP_LOGW(TAG, "Failed to mount SPIFFS partition '%s': %s",
             partition->label, esp_err_to_name(result));
    return false;
  }

  ESP_LOGI(TAG, "Mounted SPIFFS partition '%s' at %s", partition->label, BASE_PATH);
  return true;
}

void Spiffs::setup() {
  ensure_mounted();
}

float Spiffs::get_setup_priority() const {
  return setup_priority::DATA;
}

}  // namespace spiffs
}  // namespace esphome
