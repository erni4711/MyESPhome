#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/camera/camera.h"
#include "esphome/components/i2c/i2c.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef USE_ESP32
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace esphome {
namespace p4_camera {

// OV5647 MIPI-CSI output modes provided by esp_cam_sensor. The mode is fixed at
// build time (sdkconfig) and cannot be switched at runtime.
enum CameraResolution : uint8_t {
  RESOLUTION_SVGA = 0,      // legacy alias for 800x800
  RESOLUTION_VGA = 1,       // legacy alias for 800x640
  RESOLUTION_FHD = 2,       // 1920x1080 RAW10 30 fps
  RESOLUTION_800X1280 = 3,  // 800x1280 RAW8 50 fps (vendor example default)
  RESOLUTION_1280X960 = 4,  // 1280x960 RAW10 binning 45 fps
};

// Kept for API compatibility; the camera always delivers JPEG.
enum PixelFormat : uint8_t {
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
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1U << requester)) != 0;
  }

 protected:
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

 protected:
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

class P4Camera : public camera::Camera, public i2c::I2CDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // camera::Camera interface (Home Assistant API / web server)
  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new P4CameraImageReader(); }
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

  // Code generation setters
  // Only effective before setup(); the sensor mode is fixed at build time.
  void set_resolution(CameraResolution resolution) {
    if (!this->setup_started_)
      this->resolution_ = resolution;
  }
  void set_jpeg_quality(uint8_t quality) { this->jpeg_quality_ = quality; }
  void set_max_framerate(float fps) { this->max_framerate_ = fps; }
  void set_vertical_flip(bool flip) { this->vertical_flip_ = flip; }
  void set_horizontal_mirror(bool mirror) { this->horizontal_mirror_ = mirror; }
  /// Counter-clockwise output rotation in degrees (0, 90, 180, 270), done by the PPA before JPEG encoding.
  void set_rotation(uint16_t degrees) { this->rotation_ = degrees; }
  void set_reset_pin(int8_t pin) { this->reset_pin_ = pin; }
  void set_power_down_pin(int8_t pin) { this->power_down_pin_ = pin; }
  void set_sccb_frequency(uint32_t frequency) { this->sccb_frequency_ = frequency; }
  void set_standby_timeout(uint32_t timeout_ms) { this->standby_timeout_ms_ = timeout_ms; }
  void set_warmup_frames(uint8_t frames) { this->warmup_frames_ = frames; }

  /// Blocking, thread-safe capture of a fresh JPEG frame. Must not be called from the main loop
  /// for long timeouts. Returns nullptr on failure/timeout.
  std::shared_ptr<camera::CameraImage> capture_jpeg(uint32_t timeout_ms);

  // Legacy API used by the screenshot component. capture_frame() blocks until a fresh JPEG
  // frame is available; get_image_data()/get_image_size() then return that JPEG.
  bool capture_frame();
  uint8_t *get_image_data();
  size_t get_image_size();
  uint16_t get_image_width() const { return this->width_; }
  uint16_t get_image_height() const { return this->height_; }
  bool start_streaming();
  bool stop_streaming();
  bool is_streaming() const { return this->hold_streaming_.load(); }
  bool reconfigure_resolution(CameraResolution resolution);
  void set_pixel_format(int /*format*/) {}

 protected:
#ifdef USE_ESP32
  static void capture_task_(void *arg);
  void capture_loop_();
  bool initialize_video_();
  bool configure_video_device_();
  void apply_flip_controls_();
  bool stream_on_();
  void stream_off_();
  bool setup_rotation_();
  const uint8_t *rotate_frame_(const uint8_t *source);
  bool encode_jpeg_(const uint8_t *source, size_t source_length, size_t *jpeg_length);
  void deliver_frame_(const uint8_t *jpeg, size_t jpeg_length);
  bool has_pending_work_() const;
  void release_video_resources_();

  int video_fd_{-1};
  uint8_t *video_buffers_[2]{nullptr, nullptr};
  size_t video_buffer_lengths_[2]{0, 0};
  uint8_t video_buffer_count_{0};
  bool video_streaming_{false};
  bool buffers_queued_{false};
  uint32_t last_activity_ms_{0};

  jpeg_encoder_handle_t jpeg_encoder_{nullptr};
  uint8_t *jpeg_buffer_{nullptr};
  size_t jpeg_buffer_capacity_{0};

  ppa_client_handle_t ppa_client_{nullptr};
  uint8_t *rotate_buffer_{nullptr};
  size_t rotate_buffer_size_{0};

  TaskHandle_t capture_task_handle_{nullptr};
  SemaphoreHandle_t frame_lock_{nullptr};
  SemaphoreHandle_t frame_ready_{nullptr};
  SemaphoreHandle_t capture_mutex_{nullptr};
#endif

  enum class InitState : uint8_t { PENDING, READY, FAILED };
  std::atomic<InitState> init_state_{InitState::PENDING};

  // Requests coming from the camera::Camera API (main loop).
  std::atomic<uint8_t> single_requesters_{0};
  std::atomic<uint8_t> stream_requesters_{0};
  // Request handed to the capture task; result returned via pending_api_image_.
  std::atomic<uint8_t> task_api_requesters_{0};
  std::atomic<bool> api_request_in_flight_{false};
  std::shared_ptr<camera::CameraImage> pending_api_image_;
  std::shared_ptr<camera::CameraImage> current_api_image_;
  uint32_t last_api_request_ms_{0};

  // Request from capture_frame()/capture_jpeg() (any task).
  std::atomic<bool> frame_requested_{false};
  std::shared_ptr<camera::CameraImage> captured_frame_;
  // The previous frame is kept alive so a pointer obtained via get_image_data() remains valid
  // until the following capture_frame() call.
  std::shared_ptr<camera::CameraImage> last_frame_;
  std::shared_ptr<camera::CameraImage> previous_frame_;

  std::atomic<bool> hold_streaming_{false};
  bool setup_started_{false};
  std::vector<camera::CameraListener *> listeners_;

  CameraResolution resolution_{RESOLUTION_800X1280};
  uint8_t jpeg_quality_{80};
  float max_framerate_{10.0f};
  bool vertical_flip_{false};
  bool horizontal_mirror_{false};
  uint16_t rotation_{0};
  // Sensor frame size; width_/height_ are the (possibly rotated) output size.
  uint16_t sensor_width_{0};
  uint16_t sensor_height_{0};
  int8_t reset_pin_{-1};
  int8_t power_down_pin_{-1};
  uint32_t sccb_frequency_{400000};
  uint32_t standby_timeout_ms_{30000};
  uint8_t warmup_frames_{15};
  uint16_t width_{0};
  uint16_t height_{0};
};

}  // namespace p4_camera
}  // namespace esphome
