#pragma once
#include "esphome.h"
#include "esphome/components/i2c/i2c.h"
#include <functional>

// Local camera image types to avoid dependency on the top-level camera component.
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

namespace esphome {
namespace p4_camera {

enum CameraResolution {
  RESOLUTION_SVGA = 0,
  RESOLUTION_VGA = 1,
  RESOLUTION_FHD = 2,
};

enum PixelFormat {
  PIXEL_FORMAT_RGB565 = 0,
  PIXEL_FORMAT_YUV422 = 1,
  PIXEL_FORMAT_RAW8 = 2,
  PIXEL_FORMAT_JPEG = 3,
};

class P4Camera : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void loop() override {}
  void dump_config() override;
  P4Camera();

  // Legacy-style access used elsewhere
    bool capture_frame();
    void set_external_clock_pin(uint8_t pin);
    void set_external_clock_frequency(uint32_t freq);
    void set_reset_pin(GPIOPin *pin);
    void set_sensor_address(uint8_t address);
    void set_resolution(CameraResolution resolution);
    void set_pixel_format(int format);
    void set_jpeg_quality(uint8_t q);
    void set_framerate(uint8_t fps);
    void set_flip_mirror(bool v);
  uint8_t *get_image_data() { return this->frame_buffers_[0]; }
  size_t get_image_size() const { return this->frame_buffer_size_; }
  uint16_t get_image_width() const { return this->width_; }
  uint16_t get_image_height() const { return this->height_; }

  // camera API (local shim)
  bool capture(CameraImageCallback &&callback);

  // Compatibility setters expected by generated code
  void set_name(const std::string &name) { this->name_ = name; }
  void set_pwdn_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_i2c_pins(uint8_t sda, uint8_t scl) { this->i2c_sda_ = sda; this->i2c_scl_ = scl; }
  void set_i2c_address(uint8_t addr) { this->sensor_address_ = addr; }

  // Control
  bool start_streaming();
  bool stop_streaming();
  bool is_streaming() const { return this->streaming_; }
  bool reconfigure_resolution(CameraResolution new_res);

 protected:
  CameraResolution resolution_{RESOLUTION_SVGA};
  uint8_t external_clock_pin_{36};
  uint32_t external_clock_frequency_{24000000};
  GPIOPin *reset_pin_{nullptr};
  uint8_t sensor_address_{0x36};
  int pixel_format_{0};
  uint8_t jpeg_quality_{10};
  uint8_t framerate_{30};
  bool flip_mirror_{false};
  bool streaming_{false};

  // Image buffers
  uint8_t *frame_buffers_[2]{nullptr, nullptr};
  size_t frame_buffer_size_{0};
  uint16_t width_{0};
  uint16_t height_{0};
  std::string name_;
  uint8_t i2c_sda_{0};
  uint8_t i2c_scl_{0};
  bool initialized_{false};
};

}  // namespace p4_camera
}  // namespace esphome
