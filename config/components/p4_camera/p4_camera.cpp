#include "p4_camera.h"
#include "esphome.h"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <unistd.h>

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace esphome {
namespace p4_camera {

void P4Camera::dump_config() {
  ESP_LOGCONFIG("p4_camera", "P4Camera: resolution=%d streaming=%d", static_cast<int>(this->resolution_), this->streaming_);
}

P4Camera::P4Camera() {
  ESP_LOGI("p4_camera", "P4Camera constructed %p", this);
  puts("p4_camera: constructor called");
}

P4CameraImage::~P4CameraImage() {
  std::free(data_);
}

void P4CameraImageReader::set_image(
    std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::static_pointer_cast<P4CameraImage>(image);
  this->offset_ = 0;
}

size_t P4CameraImageReader::available() const {
  if (this->image_ == nullptr || this->offset_ >= this->image_->get_data_length())
    return 0;
  return this->image_->get_data_length() - this->offset_;
}

uint8_t *P4CameraImageReader::peek_data_buffer() {
  return this->image_ == nullptr ? nullptr
                                 : this->image_->get_data_buffer() + this->offset_;
}

void P4CameraImageReader::consume_data(size_t consumed) {
  this->offset_ = std::min(this->offset_ + consumed,
                           this->image_ == nullptr ? size_t{0}
                                                    : this->image_->get_data_length());
}

void P4CameraImageReader::return_image() {
  this->image_.reset();
  this->offset_ = 0;
}

void P4Camera::setup() {
  ESP_LOGI("p4_camera", "Initializing ESP32-P4 MIPI-CSI camera");
#ifdef USE_ESP32
  xTaskCreate(
      [](void *arg) {
        auto *camera = static_cast<P4Camera *>(arg);
        const bool initialized = camera->initialize_video();
        camera->initialized_.store(initialized, std::memory_order_release);
        if (!initialized)
          camera->mark_failed();
        vTaskDelete(nullptr);
      },
      "p4_camera_init", 8192, this, 5, nullptr);
#else
  this->mark_failed();
#endif
}

void P4Camera::loop() {
  const uint8_t requesters =
      this->single_requesters_.exchange(0, std::memory_order_acq_rel) |
      this->stream_requesters_.load(std::memory_order_acquire);
  if (requesters == 0 || !this->initialized_) return;
  if (this->stream_requesters_.load(std::memory_order_relaxed) != 0 &&
      millis() - this->last_stream_capture_ < 1000)
    return;

  auto image = this->capture_api_image(requesters);
  if (image == nullptr) return;
  this->last_stream_capture_ = millis();
  for (auto *listener : this->listeners_) {
    if (listener != nullptr) listener->on_camera_image(image);
  }
}

std::shared_ptr<P4CameraImage> P4Camera::capture_api_image(uint8_t requesters) {
#ifdef USE_ESP32
  if (!this->capture_video_frame() || this->jpeg_buffer_ == nullptr || this->jpeg_buffer_size_ == 0)
    return nullptr;
  auto *data = static_cast<uint8_t *>(std::malloc(this->jpeg_buffer_size_));
  if (data == nullptr) {
    ESP_LOGW("p4_camera", "Unable to allocate Home Assistant image buffer");
    return nullptr;
  }
  std::memcpy(data, this->jpeg_buffer_, this->jpeg_buffer_size_);
  return std::make_shared<P4CameraImage>(data, this->jpeg_buffer_size_, requesters);
#else
  (void) requesters;
  return nullptr;
#endif
}

void P4Camera::request_image(camera::CameraRequester requester) {
  this->single_requesters_.fetch_or(1U << requester, std::memory_order_relaxed);
}

void P4Camera::start_stream(camera::CameraRequester requester) {
  this->stream_requesters_.fetch_or(1U << requester, std::memory_order_relaxed);
  for (auto *listener : this->listeners_) {
    if (listener != nullptr) listener->on_stream_start();
  }
}

void P4Camera::stop_stream(camera::CameraRequester requester) {
  this->stream_requesters_.fetch_and(
      static_cast<uint8_t>(~(1U << requester)), std::memory_order_relaxed);
  for (auto *listener : this->listeners_) {
    if (listener != nullptr) listener->on_stream_stop();
  }
}

bool P4Camera::initialize_video() {
#ifdef USE_ESP32
  static const esp_video_init_csi_config_t csi_config = {
      .sccb_config =
          {
              .init_sccb = true,
              .i2c_config =
                  {
                      .port = 1,
                      .scl_pin = static_cast<gpio_num_t>(8),
                      .sda_pin = static_cast<gpio_num_t>(7),
                  },
              .freq = 400000,
          },
      .reset_pin = GPIO_NUM_NC,
      .pwdn_pin = GPIO_NUM_NC,
  };
  static const esp_video_init_config_t video_config = {
      .csi = &csi_config,
  };

  esp_err_t err = esp_video_init(&video_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE("p4_camera", "esp_video_init failed: %s", esp_err_to_name(err));
    return false;
  }

  this->video_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR | O_NONBLOCK);
  if (this->video_fd_ < 0) {
    ESP_LOGE("p4_camera", "Unable to open MIPI-CSI video device: %s", strerror(errno));
    return false;
  }

  if (!this->configure_video_device()) {
    this->close_video_device();
    return false;
  }

  jpeg_encode_engine_cfg_t jpeg_config{};
  jpeg_config.intr_priority = 0;
  jpeg_config.timeout_ms = 100;
  err = jpeg_new_encoder_engine(&jpeg_config, &this->jpeg_encoder_);
  if (err != ESP_OK) {
    ESP_LOGE("p4_camera", "Unable to create JPEG encoder: %s", esp_err_to_name(err));
    this->close_video_device();
    return false;
  }

  this->jpeg_buffer_capacity_ = static_cast<size_t>(this->width_) * this->height_ * 2;
  this->jpeg_buffer_ = static_cast<uint8_t *>(
      heap_caps_malloc(this->jpeg_buffer_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->jpeg_buffer_ == nullptr) {
    ESP_LOGE("p4_camera", "Unable to allocate JPEG output buffer");
    this->close_video_device();
    return false;
  }

  ESP_LOGI("p4_camera", "MIPI-CSI camera ready: %ux%u RGB565", this->width_, this->height_);
  return true;
#else
  ESP_LOGE("p4_camera", "ESP32-P4 video support requires ESP-IDF");
  return false;
#endif
}

bool P4Camera::configure_video_device() {
#ifdef USE_ESP32
  struct v4l2_format format {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_G_FMT, &format) != 0) {
    ESP_LOGE("p4_camera", "Unable to query MIPI-CSI format: %s", strerror(errno));
    return false;
  }

  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(this->video_fd_, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGE("p4_camera", "Unable to configure RGB565 output: %s", strerror(errno));
    return false;
  }

  this->width_ = format.fmt.pix.width;
  this->height_ = format.fmt.pix.height;
  this->video_buffer_size_ = format.fmt.pix.sizeimage;
  if (this->video_buffer_size_ == 0)
    this->video_buffer_size_ = static_cast<size_t>(this->width_) * this->height_ * 2;

  struct v4l2_requestbuffers request {};
  request.count = 2;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (ioctl(this->video_fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
    ESP_LOGE("p4_camera", "Unable to allocate MIPI-CSI buffers: %s", strerror(errno));
    return false;
  }

  this->video_buffer_count_ = static_cast<uint8_t>(request.count);
  for (uint8_t index = 0; index < this->video_buffer_count_; index++) {
    struct v4l2_buffer buffer {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (ioctl(this->video_fd_, VIDIOC_QUERYBUF, &buffer) != 0) {
      ESP_LOGE("p4_camera", "Unable to query MIPI-CSI buffer %u: %s", index, strerror(errno));
      return false;
    }
    this->video_buffers_[index] = static_cast<uint8_t *>(
        mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->video_fd_, buffer.m.offset));
    if (this->video_buffers_[index] == MAP_FAILED) {
      this->video_buffers_[index] = nullptr;
      ESP_LOGE("p4_camera", "Unable to map MIPI-CSI buffer %u", index);
      return false;
    }
    if (ioctl(this->video_fd_, VIDIOC_QBUF, &buffer) != 0) {
      ESP_LOGE("p4_camera", "Unable to queue MIPI-CSI buffer %u: %s", index, strerror(errno));
      return false;
    }
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE("p4_camera", "Unable to start MIPI-CSI stream: %s", strerror(errno));
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool P4Camera::capture_video_frame() {
#ifdef USE_ESP32
  struct v4l2_buffer buffer {};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  if (ioctl(this->video_fd_, VIDIOC_DQBUF, &buffer) != 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      ESP_LOGW("p4_camera", "Unable to dequeue MIPI-CSI frame: %s", strerror(errno));
    return false;
  }

  bool encoded = this->encode_jpeg(this->video_buffers_[buffer.index], buffer.bytesused);
  if (ioctl(this->video_fd_, VIDIOC_QBUF, &buffer) != 0)
    ESP_LOGW("p4_camera", "Unable to requeue MIPI-CSI buffer: %s", strerror(errno));
  return encoded;
#else
  return false;
#endif
}

bool P4Camera::encode_jpeg(const uint8_t *source, size_t source_length) {
#ifdef USE_ESP32
  jpeg_encode_cfg_t config = {
      .height = this->height_,
      .width = this->width_,
      .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
      .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
      .image_quality = this->jpeg_quality_,
      .pixel_reverse = false,
  };
  uint32_t output_size = 0;
  esp_err_t err = jpeg_encoder_process(this->jpeg_encoder_, &config, source, source_length,
                                       this->jpeg_buffer_, this->jpeg_buffer_capacity_, &output_size);
  if (err != ESP_OK) {
    ESP_LOGW("p4_camera", "JPEG encoding failed: %s", esp_err_to_name(err));
    return false;
  }
  this->jpeg_buffer_size_ = output_size;
  return output_size != 0;
#else
  return false;
#endif
}

void P4Camera::close_video_device() {
#ifdef USE_ESP32
  if (this->video_fd_ >= 0) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(this->video_fd_, VIDIOC_STREAMOFF, &type);
    close(this->video_fd_);
    this->video_fd_ = -1;
  }
  if (this->jpeg_encoder_ != nullptr) {
    jpeg_del_encoder_engine(this->jpeg_encoder_);
    this->jpeg_encoder_ = nullptr;
  }
#endif
}

bool P4Camera::capture_frame() {
  if (!this->initialized_) {
    ESP_LOGW("p4_camera", "capture_frame called before camera initialized");
    return false;
  }
  return this->capture_video_frame();
}

bool P4Camera::capture(CameraImageCallback &&callback) {
  ESP_LOGD("p4_camera", "capture() called");
  if (!this->capture_frame()) return false;
  CameraImageSpec spec;
  spec.width = this->get_image_width();
  spec.height = this->get_image_height();
  spec.format = CAMERA_IMAGE_FORMAT_RGB565; // assume RGB565 after conversion/encoding
  Buffer buffer;
  buffer.set_buffer(this->get_image_data(), this->get_image_size());
  ESP_LOGD("p4_camera", "invoking callback with image %dx%d size=%u", spec.width, spec.height, (unsigned)this->get_image_size());
  callback(&spec, &buffer);
  return true;
}

bool P4Camera::start_streaming() { this->streaming_ = true; ESP_LOGI("p4_camera","start_streaming"); return true; }
bool P4Camera::stop_streaming() { this->streaming_ = false; ESP_LOGI("p4_camera","stop_streaming"); return true; }

bool P4Camera::reconfigure_resolution(CameraResolution new_res) {
  this->resolution_ = new_res;
  ESP_LOGI("p4_camera", "set resolution %d", static_cast<int>(new_res));
  return true;
}

// Setter stubs for codegen (define the symbols so codegen calls compile).
void P4Camera::set_external_clock_pin(uint8_t pin) { this->external_clock_pin_ = pin; }
void P4Camera::set_external_clock_frequency(uint32_t freq) { this->external_clock_frequency_ = freq; }
void P4Camera::set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
void P4Camera::set_sensor_address(uint8_t address) { this->sensor_address_ = address; }
void P4Camera::set_resolution(CameraResolution resolution) { this->resolution_ = resolution; }
void P4Camera::set_pixel_format(int format) { this->pixel_format_ = format; }
void P4Camera::set_jpeg_quality(uint8_t q) { this->jpeg_quality_ = q; }
void P4Camera::set_framerate(uint8_t fps) { this->framerate_ = fps; }
void P4Camera::set_flip_mirror(bool v) { this->flip_mirror_ = v; }

}  // namespace p4_camera
}  // namespace esphome
