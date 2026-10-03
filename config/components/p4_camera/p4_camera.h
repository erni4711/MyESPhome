#pragma once
#include "esphome.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/camera/camera.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <vector>

#ifdef USE_ESP32
#include "driver/jpeg_encode.h"
#include "esp_video_device.h"
#endif

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

class P4CameraImage : public camera::CameraImage {
 public:
  P4CameraImage(uint8_t *data, size_t length, uint8_t requesters)
      : data_(data), length_(length), requesters_(requesters) {}
  ~P4CameraImage() override;
  uint8_t *get_data_buffer() override { return data_; }
  size_t get_data_length() override { return length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (requesters_ & (1U << requester)) != 0;
  }

 private:
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
};

class P4CameraImageReader : public camera::CameraImageReader {
 public:
  void set_image(std::shared_ptr<camera::CameraImage> image) override;
  size_t available() const override;
  uint8_t *peek_data_buffer() override;
  void consume_data(size_t consumed) override;
  void return_image() override;

 private:
  std::shared_ptr<P4CameraImage> image_;
  size_t offset_{0};
};

class P4Camera : public camera::Camera, public i2c::I2CDevice {
 public:
  void setup() override;
  void loop() override;
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

  void add_listener(camera::CameraListener *listener) override {
    listeners_.push_back(listener);
  }
  camera::CameraImageReader *create_image_reader() override {
    return new P4CameraImageReader();
  }
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  std::shared_ptr<P4CameraImage> capture_api_image(uint8_t requesters);
 bool initialize_video();
 bool configure_video_device();
 bool capture_video_frame();
 bool encode_jpeg(const uint8_t *source, size_t source_length);
 void close_video_device();

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
  std::atomic<bool> initialized_{false};
  std::atomic<uint8_t> single_requesters_{0};
  std::atomic<uint8_t> stream_requesters_{0};
  std::vector<camera::CameraListener *> listeners_;
  uint32_t last_stream_capture_{0};
#ifdef USE_ESP32
  int video_fd_{-1};
  jpeg_encoder_handle_t jpeg_encoder_{nullptr};
  uint8_t *video_buffers_[2]{nullptr, nullptr};
  size_t video_buffer_size_{0};
  uint8_t video_buffer_count_{0};
  uint8_t *jpeg_buffer_{nullptr};
  size_t jpeg_buffer_size_{0};
  size_t jpeg_buffer_capacity_{0};
#endif
};

}  // namespace p4_camera
}  // namespace esphome
