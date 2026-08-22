#include "p4_camera.h"
#include "esphome.h"
#include "esphome/components/web_server_base/web_server_base.h"

#ifdef USE_ESP_CAMERA
#include "esp_camera.h"
#endif

#ifdef USE_ESP32P4_ISP_CAMERA
#include "driver/isp.h"
#define HAVE_ESP32P4_ISP 1
#endif

// Some web-server APIs use `String` in signatures; alias to std::string here
using String = std::string;

namespace esphome {
namespace p4_camera {

// Early static initializer to emit a trace at program start.
static int p4_camera_static_init = []() {
  puts("p4_camera: static initializer called");
  return 0;
}();


static void map_resolution(CameraResolution res, uint16_t &w, uint16_t &h, int &frame_size_const) {
  switch (res) {
    case RESOLUTION_FHD:
      w = 1920; h = 1080; frame_size_const = 0; break;
    case RESOLUTION_VGA:
      w = 640; h = 480; frame_size_const = 1; break;
    case RESOLUTION_SVGA:
    default:
      w = 800; h = 640; frame_size_const = 2; break;
  }
}

void P4Camera::dump_config() {
  ESP_LOGCONFIG("p4_camera", "P4Camera: resolution=%d streaming=%d", static_cast<int>(this->resolution_), this->streaming_);
}

P4Camera::P4Camera() {
  ESP_LOGI("p4_camera", "P4Camera constructed %p", this);
  puts("p4_camera: constructor called");
}

void P4Camera::setup() {
  puts("p4_camera: setup called");
  ESP_LOGI("p4_camera", "P4Camera setup");
#ifdef DEBUG_P4_CAMERA
  ESP_LOGI("p4_camera", "P4Camera constructed %p", this);
  ESP_LOGI("p4_camera", "P4Camera setup start: id=%p", this);
#endif
  // Defer full camera initialization to avoid blocking startup or triggering early WDT
  // Schedule initialization ~5s after setup. This uses Component::set_timeout
  // so the call is run on the main loop/scheduler.
#ifdef USE_ESP_CAMERA
  this->set_timeout("p4_camera_init", 5000, [this]() {
    ESP_LOGI("p4_camera", "P4Camera delayed init starting");
    ESP_LOGI("p4_camera", "P4Camera delayed init starting %p", this);
    camera_config_t config;
    memset(&config, 0, sizeof(config));
    // Waveshare P4 mapping
    config.pin_pwdn = -1;
    config.pin_reset = 45;
    config.pin_xclk = 40;
    config.pin_d0 = 41;
    config.pin_d1 = 42;
    config.pin_vsync = 44;
    config.pin_href = 43;
    config.pin_pclk = -1;

    uint16_t w,h; int fs;
    map_resolution(this->resolution_, w, h, fs);
    config.xclk_freq_hz = 24000000;
    config.frame_size = static_cast<framesize_t>(fs);
#if defined(PIXFORMAT_YUV422)
    config.pixel_format = PIXFORMAT_YUV422;
#elif defined(PIXFORMAT_RGB565)
    config.pixel_format = PIXFORMAT_RGB565;
#else
    config.pixel_format = static_cast<pixformat_t>(0);
#endif
    config.fb_count = 4;
#if defined(CAMERA_ISP_MODE_AUTO)
    config.isp_mode = CAMERA_ISP_MODE_AUTO;
#endif

    ESP_LOGI("p4_camera", "Initializing esp_camera with xclk=%u frame_size=%d pixel_format=%d fb_count=%d",
             config.xclk_freq_hz, static_cast<int>(config.frame_size), static_cast<int>(config.pixel_format), config.fb_count);
#if 0
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
      ESP_LOGE("p4_camera", "esp_camera_init failed: %d (%s)", err, esp_err_to_name(err));
      ESP_LOGI("p4_camera", "Camera initialized successfully %p", this);
      this->initialized_ = false;
    } else {
      ESP_LOGI("p4_camera", "esp_camera_init OK");
      this->initialized_ = true;
    }

    
    // Register web handler for snapshots via web_server_base (AsyncWebHandler)
    if (web_server_base::global_web_server_base != nullptr) {
      class Handler : public AsyncWebHandler {
       public:
        explicit Handler(P4Camera *parent) : parent_(parent) {}
        bool canHandle(AsyncWebServerRequest *request) const override {
          if (request == nullptr) return false;
          char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
          return request->url_to(url_buf) == "/camera.jpg";
        }
        void handleRequest(AsyncWebServerRequest *request) override {
          ESP_LOGI("p4_camera", "/camera.jpg request");
          if (!this->parent_->capture_frame()) { ESP_LOGW("p4_camera", "capture_frame failed"); request->send(500, "text/plain", "Capture failed"); return; }
          // JPEG encoding not implemented yet — return 501 to indicate that
          request->send(501, "text/plain", "JPEG not implemented");
        }
        void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) override {}
        void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {}
        bool isRequestHandlerTrivial() const override { return false; }
       protected:
        P4Camera *parent_;
      };

      web_server_base::global_web_server_base->add_handler(new Handler(this));
      ESP_LOGI("p4_camera", "Registered /camera.jpg snapshot endpoint (placeholder)");
    } else {
      ESP_LOGW("p4_camera", "Web server base not available to register snapshot endpoint");
    }
#endif
  });
#endif
}

bool P4Camera::capture_frame() {
#ifdef USE_ESP_CAMERA
  if (!this->initialized_) { ESP_LOGW("p4_camera", "capture_frame called before camera initialized"); return false; }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { ESP_LOGW("p4_camera", "frame buffer is null"); return false; }
  ESP_LOGD("p4_camera", "captured fb: ptr=%p width=%d height=%d len=%d format=%d", fb, fb->width, fb->height, fb->len, fb->format);
  // Ensure our buffer is allocated
  uint16_t w,h; int fs; map_resolution(this->resolution_, w,h,fs);
  this->width_ = fb->width;
  this->height_ = fb->height;
  size_t need = fb->len;
  if (!this->frame_buffers_[0] || this->frame_buffer_size_ < need) {
    ESP_LOGI("p4_camera", "allocating frame buffer: requested=%u previous=%u", (unsigned)need, (unsigned)this->frame_buffer_size_);
    if (this->frame_buffers_[0]) { free(this->frame_buffers_[0]); this->frame_buffers_[0] = nullptr; }
    this->frame_buffers_[0] = reinterpret_cast<uint8_t*>(malloc(need));
    if (!this->frame_buffers_[0]) { ESP_LOGE("p4_camera", "malloc failed for size %u", (unsigned)need); esp_camera_fb_return(fb); return false; }
    this->frame_buffer_size_ = need;
  }
  memcpy(this->frame_buffers_[0], fb->buf, need);
  ESP_LOGD("p4_camera", "copied %u bytes to frame buffer %p", (unsigned)need, this->frame_buffers_[0]);
  esp_camera_fb_return(fb);
  return true;
#else
  return false;
#endif
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
