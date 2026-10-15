#include "p4_camera.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef USE_ESP32
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#endif

namespace esphome {
namespace p4_camera {

static const char *const TAG = "p4_camera";

static constexpr uint32_t CAPTURE_TASK_STACK_SIZE = 6144;
static constexpr UBaseType_t CAPTURE_TASK_PRIORITY = 4;
static constexpr uint32_t DQBUF_TIMEOUT_MS = 1000;
static constexpr uint32_t JPEG_ENCODE_TIMEOUT_MS = 1000;
static constexpr uint8_t MAX_DQBUF_ERRORS = 3;

P4CameraImage::~P4CameraImage() { std::free(this->data_); }

void P4CameraImageReader::set_image(std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::move(image);
  this->offset_ = 0;
}

size_t P4CameraImageReader::available() const {
  if (this->image_ == nullptr)
    return 0;
  const size_t length = this->image_->get_data_length();
  return this->offset_ >= length ? 0 : length - this->offset_;
}

uint8_t *P4CameraImageReader::peek_data_buffer() {
  return this->image_ == nullptr ? nullptr : this->image_->get_data_buffer() + this->offset_;
}

void P4CameraImageReader::consume_data(size_t consumed) {
  if (this->image_ == nullptr)
    return;
  this->offset_ = std::min(this->offset_ + consumed, this->image_->get_data_length());
}

void P4CameraImageReader::return_image() {
  this->image_.reset();
  this->offset_ = 0;
}

static const char *resolution_to_string(CameraResolution resolution) {
  switch (resolution) {
    case RESOLUTION_VGA:
      return "800x640 RAW8 50fps";
    case RESOLUTION_SVGA:
      return "800x800 RAW8 50fps";
    case RESOLUTION_800X1280:
      return "800x1280 RAW8 50fps";
    case RESOLUTION_1280X960:
      return "1280x960 RAW10 binning 45fps";
    case RESOLUTION_FHD:
      return "1920x1080 RAW10 30fps";
  }
  return "unknown";
}

void P4Camera::dump_config() {
  ESP_LOGCONFIG(TAG, "P4 MIPI-CSI Camera:");
  ESP_LOGCONFIG(TAG, "  Sensor mode: %s", resolution_to_string(this->resolution_));
  ESP_LOGCONFIG(TAG, "  Output: %ux%u JPEG, quality %u", this->width_, this->height_, this->jpeg_quality_);
  ESP_LOGCONFIG(TAG, "  Rotation: %u° counter-clockwise", this->rotation_);
  ESP_LOGCONFIG(TAG, "  Max stream framerate: %.1f fps", this->max_framerate_);
  ESP_LOGCONFIG(TAG, "  Vertical flip: %s, horizontal mirror: %s", YESNO(this->vertical_flip_),
                YESNO(this->horizontal_mirror_));
  ESP_LOGCONFIG(TAG, "  Standby timeout: %u ms, warm-up frames: %u", (unsigned) this->standby_timeout_ms_,
                this->warmup_frames_);
  LOG_I2C_DEVICE(this);
  if (this->init_state_.load() == InitState::FAILED)
    ESP_LOGE(TAG, "  Camera initialization failed");
}

void P4Camera::setup() {
  this->setup_started_ = true;
#ifdef USE_ESP32
  this->frame_lock_ = xSemaphoreCreateMutex();
  this->capture_mutex_ = xSemaphoreCreateMutex();
  this->frame_ready_ = xSemaphoreCreateBinary();
  if (this->frame_lock_ == nullptr || this->capture_mutex_ == nullptr || this->frame_ready_ == nullptr) {
    ESP_LOGE(TAG, "Unable to create synchronization primitives");
    this->mark_failed();
    return;
  }
  // esp_video_init() probes the sensor and loads the IPA configuration, which takes a while;
  // do it in the capture task so the main loop is not blocked.
  if (xTaskCreatePinnedToCore(P4Camera::capture_task_, "p4_camera", CAPTURE_TASK_STACK_SIZE, this,
                              CAPTURE_TASK_PRIORITY, &this->capture_task_handle_, tskNO_AFFINITY) != pdPASS) {
    ESP_LOGE(TAG, "Unable to create capture task");
    this->mark_failed();
  }
#else
  ESP_LOGE(TAG, "ESP32-P4 MIPI-CSI camera requires ESP-IDF");
  this->mark_failed();
#endif
}

void P4Camera::loop() {
  const InitState state = this->init_state_.load(std::memory_order_acquire);
  if (state == InitState::PENDING)
    return;
  if (state == InitState::FAILED) {
    this->mark_failed();
    return;
  }

#ifdef USE_ESP32
  std::shared_ptr<camera::CameraImage> image;
  xSemaphoreTake(this->frame_lock_, portMAX_DELAY);
  image.swap(this->pending_api_image_);
  xSemaphoreGive(this->frame_lock_);
  if (image != nullptr) {
    this->current_api_image_ = image;
    this->api_request_in_flight_.store(false);
    for (auto *listener : this->listeners_)
      listener->on_camera_image(image);
  }

  if (this->api_request_in_flight_.load())
    return;
  // Wait until all API connections have finished sending the previous image.
  if (this->current_api_image_ != nullptr) {
    if (this->current_api_image_.use_count() > 1)
      return;
    this->current_api_image_.reset();
  }

  const uint32_t now = millis();
  uint8_t requesters = this->single_requesters_.exchange(0);
  const uint8_t streams = this->stream_requesters_.load();
  if (streams != 0) {
    const uint32_t interval_ms = static_cast<uint32_t>(1000.0f / this->max_framerate_);
    if (now - this->last_api_request_ms_ >= interval_ms)
      requesters |= streams;
  }
  if (requesters == 0)
    return;

  this->last_api_request_ms_ = now;
  this->api_request_in_flight_.store(true);
  this->task_api_requesters_.store(requesters);
  xTaskNotifyGive(this->capture_task_handle_);
#endif
}

void P4Camera::request_image(camera::CameraRequester requester) {
  this->single_requesters_.fetch_or(static_cast<uint8_t>(1U << requester));
}

void P4Camera::start_stream(camera::CameraRequester requester) {
  this->stream_requesters_.fetch_or(static_cast<uint8_t>(1U << requester));
  for (auto *listener : this->listeners_)
    listener->on_stream_start();
}

void P4Camera::stop_stream(camera::CameraRequester requester) {
  this->stream_requesters_.fetch_and(static_cast<uint8_t>(~(1U << requester)));
  for (auto *listener : this->listeners_)
    listener->on_stream_stop();
}

std::shared_ptr<camera::CameraImage> P4Camera::capture_jpeg(uint32_t timeout_ms) {
#ifdef USE_ESP32
  if (this->init_state_.load(std::memory_order_acquire) != InitState::READY)
    return nullptr;
  if (xTaskGetCurrentTaskHandle() == this->capture_task_handle_)
    return nullptr;
  if (xSemaphoreTake(this->capture_mutex_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    return nullptr;

  // Drop a late result/signal from a previous request that timed out.
  xSemaphoreTake(this->frame_ready_, 0);
  xSemaphoreTake(this->frame_lock_, portMAX_DELAY);
  this->captured_frame_.reset();
  xSemaphoreGive(this->frame_lock_);

  this->frame_requested_.store(true);
  xTaskNotifyGive(this->capture_task_handle_);
  xSemaphoreTake(this->frame_ready_, pdMS_TO_TICKS(timeout_ms));
  this->frame_requested_.store(false);

  std::shared_ptr<camera::CameraImage> result;
  xSemaphoreTake(this->frame_lock_, portMAX_DELAY);
  result.swap(this->captured_frame_);
  xSemaphoreGive(this->frame_lock_);
  xSemaphoreGive(this->capture_mutex_);
  return result;
#else
  (void) timeout_ms;
  return nullptr;
#endif
}

bool P4Camera::capture_frame() {
  // Covers stream start-up, sensor warm-up frames and JPEG encoding.
  const uint32_t timeout_ms = 2000 + this->warmup_frames_ * 40U;
  auto frame = this->capture_jpeg(timeout_ms);
  if (frame == nullptr)
    return false;
#ifdef USE_ESP32
  xSemaphoreTake(this->frame_lock_, portMAX_DELAY);
  this->previous_frame_ = std::move(this->last_frame_);
  this->last_frame_ = std::move(frame);
  xSemaphoreGive(this->frame_lock_);
#endif
  return true;
}

uint8_t *P4Camera::get_image_data() {
  return this->last_frame_ == nullptr ? nullptr : this->last_frame_->get_data_buffer();
}

size_t P4Camera::get_image_size() {
  return this->last_frame_ == nullptr ? 0 : this->last_frame_->get_data_length();
}

bool P4Camera::start_streaming() {
  this->hold_streaming_.store(true);
#ifdef USE_ESP32
  if (this->capture_task_handle_ != nullptr && this->init_state_.load() == InitState::READY)
    xTaskNotifyGive(this->capture_task_handle_);
#endif
  return true;
}

bool P4Camera::stop_streaming() {
  this->hold_streaming_.store(false);
  return true;
}

bool P4Camera::reconfigure_resolution(CameraResolution resolution) {
  if (resolution == this->resolution_)
    return true;
  ESP_LOGW(TAG, "Runtime resolution change to '%s' is not supported; sensor mode is fixed to '%s'",
           resolution_to_string(resolution), resolution_to_string(this->resolution_));
  return false;
}

#ifdef USE_ESP32

void P4Camera::capture_task_(void *arg) {
  auto *camera = static_cast<P4Camera *>(arg);
  if (!camera->initialize_video_()) {
    camera->release_video_resources_();
    camera->init_state_.store(InitState::FAILED, std::memory_order_release);
    vTaskDelete(nullptr);
    return;
  }
  camera->init_state_.store(InitState::READY, std::memory_order_release);
  camera->capture_loop_();
}

bool P4Camera::initialize_video_() {
  if (this->bus_ == nullptr) {
    ESP_LOGE(TAG, "No I2C bus configured");
    return false;
  }
  // Share ESPHome's I2C master bus with esp_video (SCCB) instead of creating a second controller
  // on the same pins. This requires a native (non-multiplexed) bus.
  const int port = static_cast<i2c::InternalI2CBus *>(this->bus_)->get_port();
  i2c_master_bus_handle_t i2c_handle = nullptr;
  esp_err_t err = i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(port), &i2c_handle);
  if (err != ESP_OK || i2c_handle == nullptr) {
    ESP_LOGE(TAG, "Unable to get I2C bus handle for port %d: %s", port, esp_err_to_name(err));
    return false;
  }

  esp_video_init_csi_config_t csi_config{};
  csi_config.sccb_config.init_sccb = false;
  csi_config.sccb_config.i2c_handle = i2c_handle;
  csi_config.sccb_config.freq = this->sccb_frequency_;
  csi_config.reset_pin = static_cast<gpio_num_t>(this->reset_pin_);
  csi_config.pwdn_pin = static_cast<gpio_num_t>(this->power_down_pin_);

  esp_video_init_config_t video_config{};
  video_config.csi = &csi_config;

  err = esp_video_init(&video_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
    return false;
  }

  this->video_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (this->video_fd_ < 0) {
    ESP_LOGE(TAG, "Unable to open %s (camera sensor not detected?): %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
             strerror(errno));
    return false;
  }

  if (!this->configure_video_device_())
    return false;

  if (!this->setup_rotation_())
    return false;

  jpeg_encode_engine_cfg_t engine_config{};
  engine_config.intr_priority = 0;
  engine_config.timeout_ms = JPEG_ENCODE_TIMEOUT_MS;
  err = jpeg_new_encoder_engine(&engine_config, &this->jpeg_encoder_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Unable to create JPEG encoder: %s", esp_err_to_name(err));
    return false;
  }

  // The JPEG output buffer must be cache-line aligned; use the driver allocator.
  jpeg_encode_memory_alloc_cfg_t mem_config{};
  mem_config.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  const size_t requested = static_cast<size_t>(this->width_) * this->height_ * 2;
  this->jpeg_buffer_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(requested, &mem_config,
                                                                     &this->jpeg_buffer_capacity_));
  if (this->jpeg_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Unable to allocate %u byte JPEG output buffer", (unsigned) requested);
    return false;
  }

  ESP_LOGI(TAG, "Camera ready: %ux%u (%s)", this->width_, this->height_, resolution_to_string(this->resolution_));
  return true;
}

bool P4Camera::configure_video_device_() {
  struct v4l2_capability capability {};
  if (ioctl(this->video_fd_, VIDIOC_QUERYCAP, &capability) == 0) {
    ESP_LOGD(TAG, "Video device: driver=%s card=%s", capability.driver, capability.card);
  }

  struct v4l2_format format {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_G_FMT, &format) != 0) {
    ESP_LOGE(TAG, "Unable to query video format: %s", strerror(errno));
    return false;
  }
  if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
    const uint32_t width = format.fmt.pix.width;
    const uint32_t height = format.fmt.pix.height;
    format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(this->video_fd_, VIDIOC_S_FMT, &format) != 0) {
      ESP_LOGE(TAG, "Unable to select RGB565 output: %s", strerror(errno));
      return false;
    }
  }
  this->sensor_width_ = static_cast<uint16_t>(format.fmt.pix.width);
  this->sensor_height_ = static_cast<uint16_t>(format.fmt.pix.height);
  const bool swap = this->rotation_ == 90 || this->rotation_ == 270;
  this->width_ = swap ? this->sensor_height_ : this->sensor_width_;
  this->height_ = swap ? this->sensor_width_ : this->sensor_height_;

  // esp_video ignores O_NONBLOCK; without a timeout VIDIOC_DQBUF would block forever if the sensor stalls.
  struct timeval timeout {};
  timeout.tv_sec = DQBUF_TIMEOUT_MS / 1000;
  timeout.tv_usec = (DQBUF_TIMEOUT_MS % 1000) * 1000;
  if (ioctl(this->video_fd_, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0)
    ESP_LOGW(TAG, "Unable to set DQBUF timeout: %s", strerror(errno));

  this->apply_flip_controls_();

  struct v4l2_requestbuffers request {};
  request.count = 2;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (ioctl(this->video_fd_, VIDIOC_REQBUFS, &request) != 0) {
    ESP_LOGE(TAG, "Unable to allocate video buffers: %s", strerror(errno));
    return false;
  }
  this->video_buffer_count_ = std::min<uint32_t>(request.count, 2);
  if (this->video_buffer_count_ < 2) {
    ESP_LOGE(TAG, "Driver provided only %u video buffer(s)", this->video_buffer_count_);
    return false;
  }

  for (uint8_t index = 0; index < this->video_buffer_count_; index++) {
    struct v4l2_buffer buffer {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (ioctl(this->video_fd_, VIDIOC_QUERYBUF, &buffer) != 0) {
      ESP_LOGE(TAG, "Unable to query video buffer %u: %s", index, strerror(errno));
      return false;
    }
    void *mapped = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->video_fd_, buffer.m.offset);
    if (mapped == MAP_FAILED || mapped == nullptr) {
      ESP_LOGE(TAG, "Unable to map video buffer %u", index);
      return false;
    }
    this->video_buffers_[index] = static_cast<uint8_t *>(mapped);
    this->video_buffer_lengths_[index] = buffer.length;
  }
  return true;
}

void P4Camera::apply_flip_controls_() {
  const struct {
    uint32_t id;
    bool enabled;
    const char *name;
  } flips[] = {
      {V4L2_CID_VFLIP, this->vertical_flip_, "vertical flip"},
      {V4L2_CID_HFLIP, this->horizontal_mirror_, "horizontal mirror"},
  };
  for (const auto &flip : flips) {
    if (!flip.enabled)
      continue;
    struct v4l2_ext_control control {};
    control.id = flip.id;
    control.value = 1;
    struct v4l2_ext_controls controls {};
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(this->video_fd_, VIDIOC_S_EXT_CTRLS, &controls) != 0)
      ESP_LOGW(TAG, "Unable to enable %s: %s", flip.name, strerror(errno));
  }
}

bool P4Camera::stream_on_() {
  // esp_video only resets its buffer queues on a successful STREAMOFF, so buffers stay queued
  // after a failed STREAMON and must not be queued again.
  if (!this->buffers_queued_) {
    for (uint8_t index = 0; index < this->video_buffer_count_; index++) {
      struct v4l2_buffer buffer {};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      if (ioctl(this->video_fd_, VIDIOC_QBUF, &buffer) != 0) {
        ESP_LOGE(TAG, "Unable to queue video buffer %u: %s", index, strerror(errno));
        return false;
      }
    }
    this->buffers_queued_ = true;
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "Unable to start video stream: %s", strerror(errno));
    return false;
  }
  this->video_streaming_ = true;
  ESP_LOGD(TAG, "Video stream started");
  return true;
}

void P4Camera::stream_off_() {
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_STREAMOFF, &type) != 0) {
    ESP_LOGW(TAG, "Unable to stop video stream: %s", strerror(errno));
  } else {
    this->buffers_queued_ = false;
  }
  this->video_streaming_ = false;
  ESP_LOGD(TAG, "Video stream stopped");
}

bool P4Camera::has_pending_work_() const {
  return this->task_api_requesters_.load() != 0 || this->frame_requested_.load() || this->hold_streaming_.load();
}

void P4Camera::capture_loop_() {
  uint8_t frames_to_skip = 0;
  uint8_t dqbuf_errors = 0;

  while (true) {
    const bool work = this->has_pending_work_();
    if (!this->video_streaming_) {
      if (!work) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        continue;
      }
      if (!this->stream_on_()) {
        this->deliver_frame_(nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      // Let auto exposure / white balance settle before handing out images.
      frames_to_skip = this->warmup_frames_;
      dqbuf_errors = 0;
      this->last_activity_ms_ = millis();
    } else if (work) {
      this->last_activity_ms_ = millis();
    } else if (this->standby_timeout_ms_ != 0 && millis() - this->last_activity_ms_ >= this->standby_timeout_ms_) {
      this->stream_off_();
      continue;
    }

    struct v4l2_buffer buffer {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl(this->video_fd_, VIDIOC_DQBUF, &buffer) != 0) {
      if (++dqbuf_errors >= MAX_DQBUF_ERRORS) {
        ESP_LOGW(TAG, "No frames received from camera, restarting stream");
        this->stream_off_();
        this->deliver_frame_(nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      continue;
    }
    dqbuf_errors = 0;

    const bool frame_ok = buffer.index < this->video_buffer_count_ && buffer.bytesused != 0 &&
                          (buffer.flags & V4L2_BUF_FLAG_ERROR) == 0;
    const bool wanted = this->task_api_requesters_.load() != 0 || this->frame_requested_.load();
    const bool process = wanted && frame_ok && frames_to_skip == 0;
    if (frames_to_skip > 0)
      frames_to_skip--;
    size_t jpeg_length = 0;
    const bool encoded =
        process && this->encode_jpeg_(this->video_buffers_[buffer.index], buffer.bytesused, &jpeg_length);

    if (ioctl(this->video_fd_, VIDIOC_QBUF, &buffer) != 0)
      ESP_LOGW(TAG, "Unable to requeue video buffer %u: %s", (unsigned) buffer.index, strerror(errno));

    if (process)
      this->deliver_frame_(encoded ? this->jpeg_buffer_ : nullptr, encoded ? jpeg_length : 0);
  }
}

bool P4Camera::setup_rotation_() {
  if (this->rotation_ == 0)
    return true;

  ppa_client_config_t client_config{};
  client_config.oper_type = PPA_OPERATION_SRM;
  esp_err_t err = ppa_register_client(&client_config, &this->ppa_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Unable to register PPA client for rotation: %s", esp_err_to_name(err));
    return false;
  }

  // The PPA output buffer address and size must be cache-line aligned.
  size_t alignment = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment) != ESP_OK || alignment == 0)
    alignment = 128;
  const size_t frame_size = static_cast<size_t>(this->sensor_width_) * this->sensor_height_ * 2;
  this->rotate_buffer_size_ = (frame_size + alignment - 1) & ~(alignment - 1);
  this->rotate_buffer_ = static_cast<uint8_t *>(
      heap_caps_aligned_calloc(alignment, 1, this->rotate_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->rotate_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Unable to allocate %u byte rotation buffer", (unsigned) this->rotate_buffer_size_);
    return false;
  }
  return true;
}

const uint8_t *P4Camera::rotate_frame_(const uint8_t *source) {
  if (this->ppa_client_ == nullptr)
    return source;

  ppa_srm_oper_config_t config{};
  config.in.buffer = source;
  config.in.pic_w = this->sensor_width_;
  config.in.pic_h = this->sensor_height_;
  config.in.block_w = this->sensor_width_;
  config.in.block_h = this->sensor_height_;
  config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  config.out.buffer = this->rotate_buffer_;
  config.out.buffer_size = this->rotate_buffer_size_;
  config.out.pic_w = this->width_;
  config.out.pic_h = this->height_;
  config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  switch (this->rotation_) {
    case 90:
      config.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
      break;
    case 180:
      config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
      break;
    case 270:
      config.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
      break;
    default:
      config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
      break;
  }
  config.scale_x = 1.0f;
  config.scale_y = 1.0f;
  config.mode = PPA_TRANS_MODE_BLOCKING;

  esp_err_t err = ppa_do_scale_rotate_mirror(this->ppa_client_, &config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PPA rotation failed: %s", esp_err_to_name(err));
    return nullptr;
  }
  return this->rotate_buffer_;
}

bool P4Camera::encode_jpeg_(const uint8_t *source, size_t source_length, size_t *jpeg_length) {
  const size_t expected = static_cast<size_t>(this->sensor_width_) * this->sensor_height_ * 2;
  if (source_length < expected) {
    ESP_LOGW(TAG, "Incomplete frame (%u of %u bytes)", (unsigned) source_length, (unsigned) expected);
    return false;
  }

  source = this->rotate_frame_(source);
  if (source == nullptr)
    return false;

  jpeg_encode_cfg_t config{};
  config.width = this->width_;
  config.height = this->height_;
  config.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  config.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
  config.image_quality = this->jpeg_quality_;

  uint32_t output_size = 0;
  esp_err_t err = jpeg_encoder_process(this->jpeg_encoder_, &config, source, expected, this->jpeg_buffer_,
                                       this->jpeg_buffer_capacity_, &output_size);
  if (err != ESP_OK || output_size == 0) {
    ESP_LOGW(TAG, "JPEG encoding failed: %s", esp_err_to_name(err));
    return false;
  }
  *jpeg_length = output_size;
  return true;
}

void P4Camera::deliver_frame_(const uint8_t *jpeg, size_t jpeg_length) {
  const uint8_t requesters = this->task_api_requesters_.exchange(0);
  const bool frame_requested = this->frame_requested_.exchange(false);
  if (requesters == 0 && !frame_requested)
    return;

  std::shared_ptr<camera::CameraImage> image;
  if (jpeg != nullptr && jpeg_length != 0) {
    auto *data = static_cast<uint8_t *>(heap_caps_malloc(jpeg_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr)
      data = static_cast<uint8_t *>(std::malloc(jpeg_length));
    if (data != nullptr) {
      std::memcpy(data, jpeg, jpeg_length);
      image = std::make_shared<P4CameraImage>(data, jpeg_length, requesters);
    } else {
      ESP_LOGW(TAG, "Unable to allocate %u byte image buffer", (unsigned) jpeg_length);
    }
  }

  xSemaphoreTake(this->frame_lock_, portMAX_DELAY);
  if (frame_requested)
    this->captured_frame_ = image;
  if (requesters != 0 && image != nullptr)
    this->pending_api_image_ = image;
  xSemaphoreGive(this->frame_lock_);

  if (requesters != 0 && image == nullptr)
    this->api_request_in_flight_.store(false);
  if (frame_requested)
    xSemaphoreGive(this->frame_ready_);
}

void P4Camera::release_video_resources_() {
  if (this->video_fd_ >= 0) {
    if (this->video_streaming_)
      this->stream_off_();
    close(this->video_fd_);
    this->video_fd_ = -1;
  }
  if (this->jpeg_encoder_ != nullptr) {
    jpeg_del_encoder_engine(this->jpeg_encoder_);
    this->jpeg_encoder_ = nullptr;
  }
  if (this->jpeg_buffer_ != nullptr) {
    heap_caps_free(this->jpeg_buffer_);
    this->jpeg_buffer_ = nullptr;
    this->jpeg_buffer_capacity_ = 0;
  }
  if (this->ppa_client_ != nullptr) {
    ppa_unregister_client(this->ppa_client_);
    this->ppa_client_ = nullptr;
  }
  if (this->rotate_buffer_ != nullptr) {
    heap_caps_free(this->rotate_buffer_);
    this->rotate_buffer_ = nullptr;
    this->rotate_buffer_size_ = 0;
  }
}

#endif  // USE_ESP32

}  // namespace p4_camera
}  // namespace esphome
