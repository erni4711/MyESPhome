#include "lvgl_camera_display.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace lvgl_camera_display {

static const char *const TAG = "lvgl_camera_display";

void LVGLCameraDisplay::setup() {
  ESP_LOGCONFIG(TAG, "🎥 Configuration LVGL Camera Display...");

  if (this->camera_ == nullptr) {
    ESP_LOGE(TAG, "❌ Camera non configurée");
    this->mark_failed();
    return;
  }

  // Intervalle pour 30 FPS
  this->update_interval_ = 33;  // ms

  ESP_LOGI(TAG, "✅ LVGL Camera Display initialisé");
  ESP_LOGI(TAG, "   Update interval: %u ms (~%d FPS)", 
           this->update_interval_, 1000 / this->update_interval_);
}

void LVGLCameraDisplay::loop() {
  uint32_t now = millis();

  // Vérifier si c'est le moment de mettre à jour
  if (now - this->last_update_ < this->update_interval_) {
    return;
  }

  this->last_update_ = now;

  // Si la caméra est en streaming, capturer ET mettre à jour le canvas
  if (this->camera_->is_streaming()) {
    bool frame_captured = this->camera_->capture_frame();

    // Debug: log capture result and current image buffer pointer/size
    uint8_t *dbg_buf = this->camera_->get_image_data();
    uint16_t dbg_w = this->camera_->get_image_width();
    uint16_t dbg_h = this->camera_->get_image_height();
    ESP_LOGD(TAG, "Streaming active - capture_frame()=%d, img=%p (%ux%u)",
             frame_captured, dbg_buf, dbg_w, dbg_h);

    if (frame_captured) {
      this->update_canvas_();
      this->frame_count_++;

      // Logger FPS réel toutes les 100 frames
      if (this->frame_count_ % 100 == 0) {
        static uint32_t last_time = 0;
        uint32_t now_time = millis();

        if (last_time > 0) {
          float elapsed = (now_time - last_time) / 1000.0f;  // secondes
          float fps = 100.0f / elapsed;
          ESP_LOGI(TAG, "🎞️ %u frames affichées - FPS moyen: %.2f", this->frame_count_, fps);
        }
        last_time = now_time;
      }
    }
  }
}

void LVGLCameraDisplay::dump_config() {
  ESP_LOGCONFIG(TAG, "LVGL Camera Display:");
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->update_interval_);
  ESP_LOGCONFIG(TAG, "  FPS cible: ~%d", 1000 / this->update_interval_);
  ESP_LOGCONFIG(TAG, "  Canvas configuré: %s", this->canvas_obj_ ? "OUI" : "NON");
}

void LVGLCameraDisplay::update_canvas_() {
  if (this->camera_ == nullptr) {
    return;
  }

  if (this->canvas_obj_ == nullptr) {
    if (!this->canvas_warning_shown_) {
      ESP_LOGW(TAG, "❌ Canvas null - pas encore configuré?");
      this->canvas_warning_shown_ = true;
    }
    return;
  }

  uint8_t *img_data = this->camera_->get_image_data();
  uint16_t width = this->camera_->get_image_width();
  uint16_t height = this->camera_->get_image_height();

  if (img_data == nullptr) {
    return;
  }

  if (this->first_update_) {
    ESP_LOGI(TAG, "🖼️  Premier update canvas:");
    ESP_LOGI(TAG, "   Dimensions: %ux%u", width, height);
    ESP_LOGI(TAG, "   Buffer: %p", img_data);
    ESP_LOGI(TAG, "   Premiers pixels (RGB565): %02X%02X %02X%02X %02X%02X", 
             img_data[0], img_data[1], img_data[2], img_data[3], img_data[4], img_data[5]);
    this->first_update_ = false;
  }

#if LV_USE_CANVAS
  lv_canvas_set_buffer(this->canvas_obj_, img_data, width, height, LV_IMG_CF_TRUE_COLOR);
  lv_obj_invalidate(this->canvas_obj_);
#else
  (void)img_data; (void)width; (void)height;
  lv_obj_invalidate(this->canvas_obj_);
#endif
}

void LVGLCameraDisplay::configure_canvas(lv_obj_t *canvas) { 
  this->canvas_obj_ = canvas;
  ESP_LOGI(TAG, "🎨 Canvas configuré: %p", canvas);

  if (canvas != nullptr) {
    lv_coord_t w = lv_obj_get_width(canvas);
    lv_coord_t h = lv_obj_get_height(canvas);
    ESP_LOGI(TAG, "   Taille canvas: %dx%d", w, h);

    const uint16_t target_w = 800;
    const uint16_t target_h = 640;
    uint16_t use_w = (w > 0) ? static_cast<uint16_t>(w) : target_w;
    uint16_t use_h = (h > 0) ? static_cast<uint16_t>(h) : target_h;
    if (use_w > target_w) use_w = target_w;
    if (use_h > target_h) use_h = target_h;

    if (static_cast<uint16_t>(w) != use_w || static_cast<uint16_t>(h) != use_h) {
      lv_obj_set_size(canvas, use_w, use_h);
      ESP_LOGI(TAG, "   Taille canvas ajustée: %ux%u", use_w, use_h);
    }

    if (use_w > 0 && use_h > 0) {
      // Canvas size adjusted; no extra downscale buffer used.
    }
  }
}

}  // namespace lvgl_camera_display
}  // namespace esphome

