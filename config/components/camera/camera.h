#pragma once
#include <functional>
#include <cstdint>
#include <cstddef>

namespace esphome {
namespace camera {

struct CameraImageSpec {
  uint16_t width;
  uint16_t height;
  int format;
};

class Buffer {
 public:
  void set_buffer(uint8_t *data, size_t len) { this->data_ = data; this->len_ = len; }
  uint8_t *get_data() { return this->data_; }
  size_t get_length() const { return this->len_; }
 private:
  uint8_t *data_{nullptr};
  size_t len_{0};
};

using CameraImageCallback = std::function<void(CameraImageSpec *, Buffer *)>;

constexpr int CAMERA_IMAGE_FORMAT_RGB565 = 1;

}  // namespace camera
}  // namespace esphome
