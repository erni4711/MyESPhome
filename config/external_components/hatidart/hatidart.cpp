#include "hatidart.h"
#include "../hatifonts/ui_fonts.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <lvgl.h>
#include <array>

#include "esphome/components/spiffs/spiffs.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace hatidart {

static const char *const TAG = "hatidart";
static constexpr int ACTION_MULTIPLIER = 1;
static constexpr int ACTION_DART_VALUE = 2;
static constexpr int ACTION_SUBMIT = 3;
static constexpr int ACTION_RESET = 4;
static constexpr int ACTION_CLOSE = 5;
static constexpr int ACTION_CORRECTION = 6;
static constexpr int ACTION_FAIL = 7;
static constexpr int ACTION_DOUBLE_OUT = 8;
static constexpr int ACTION_PLAYER_COUNT = 9;
static HATiDart *g_hatidart = nullptr;

void draw_dartboard_segments(lv_event_t *event) {
  if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
  lv_obj_t *object = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
  lv_layer_t *layer = lv_event_get_layer(event);
  lv_area_t area;
  lv_obj_get_coords(object, &area);
  const lv_coord_t width = lv_area_get_width(&area);
  const lv_coord_t height = lv_area_get_height(&area);
  const lv_coord_t radius = std::min(width, height) / 2 - 2;
  const lv_point_t center = {
      static_cast<lv_coord_t>((area.x1 + area.x2) / 2),
      static_cast<lv_coord_t>((area.y1 + area.y2) / 2),
  };
  const lv_color_t cream = lv_color_make(0xE8, 0xD0, 0x86);
  const lv_color_t black = lv_color_make(0x18, 0x18, 0x18);
  const lv_color_t red = lv_color_make(0xC8, 0x32, 0x32);
  const lv_color_t green = lv_color_make(0x28, 0x8A, 0x55);
  const lv_color_t dim_red = lv_color_make(0xC8, 0x32, 0x32);
  const lv_color_t dim_green = lv_color_make(0x28, 0x8A, 0x55);

  auto draw_band = [&](float radius_ratio, float width_ratio,
                       lv_color_t first, lv_color_t second, bool active) {
    for (int index = 0; index < 20; ++index) {
      lv_draw_arc_dsc_t arc;
      lv_draw_arc_dsc_init(&arc);
      arc.center = center;
      arc.radius = static_cast<uint16_t>(radius * radius_ratio);
      arc.width = static_cast<int32_t>(radius * width_ratio);
      const int start_angle = (261 + index * 18) % 360;
      arc.start_angle = start_angle;
      arc.end_angle = (start_angle + 16) % 360;
      arc.color = index % 2 == 0 ? first : second;
      arc.opa = active ? LV_OPA_COVER : LV_OPA_50;
      lv_draw_arc(layer, &arc);
    }
  };

  const int multiplier = g_hatidart == nullptr ? 0 : g_hatidart->selected_multiplier();
  const lv_color_t active_first = lv_color_make(0x8A, 0xD6, 0x8A);
  const lv_color_t active_second = lv_color_make(0xFF, 0x9A, 0x9A);
  const lv_color_t active_bull = lv_color_make(0xFF, 0xC1, 0x28);
  const lv_color_t active_bull_eye = lv_color_make(0xFF, 0x5A, 0x5F);
  // WDF/PDC steel-tip dimensions: 451 mm board diameter, 8 mm double/treble
  // ring width, 340 mm double-ring outer diameter, and 214 mm treble-ring
  // outer diameter. Ratios are relative to the 225.5 mm board radius.
  // LVGL's arc radius is the outer edge of the stroke.
  constexpr float ring_width = 8.0f / 225.5f;
  constexpr float double_radius = 170.0f / 225.5f;
  constexpr float outer_single_inner = 109.0f;
  constexpr float outer_single_outer = 160.0f;
  constexpr float outer_single_radius = outer_single_outer / 225.5f;
  constexpr float treble_radius = 107.0f / 225.5f;
  constexpr float inner_single_inner = 17.9f;
  constexpr float inner_single_outer = 97.0f;
  constexpr float inner_single_radius = inner_single_outer / 225.5f;
  const float outer_single_width =
      (outer_single_outer - outer_single_inner) / 225.5f;
  const float inner_single_width =
      (inner_single_outer - inner_single_inner) / 225.5f;
  draw_band(double_radius, ring_width, green, red, multiplier == 2);
  draw_band(outer_single_radius, outer_single_width, cream, black, multiplier == 1);
  draw_band(treble_radius, ring_width, green, red, multiplier == 3);
  draw_band(inner_single_radius, inner_single_width, cream, black, multiplier == 1);
  if (multiplier == 2) {
    draw_band(double_radius, ring_width, active_first, active_second, true);
  } else if (multiplier == 3) {
    draw_band(treble_radius, ring_width, active_first, active_second, true);
  } else if (multiplier == 1) {
    draw_band(outer_single_radius, outer_single_width, active_first, active_second, true);
    draw_band(inner_single_radius, inner_single_width, active_first, active_second, true);
  }
  const bool bull_active = multiplier == 1 || multiplier == 2;
  const bool bull_eye_active = multiplier == 2;
  auto draw_bull = [&](float diameter_ratio, lv_color_t color) {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center = center;
    arc.radius = static_cast<uint16_t>(radius * diameter_ratio / 2.0f);
    arc.width = static_cast<int32_t>(radius * diameter_ratio);
    arc.start_angle = 0;
    arc.end_angle = 359;
    arc.color = color;
    arc.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &arc);
  };
  // Official bull diameters are 31.8 mm outer bull and 12.7 mm inner bull
  // on a 451 mm board.
  constexpr float outer_bull_diameter = 31.8f / 451.0f;
  constexpr float inner_bull_diameter = 12.7f / 451.0f;
  draw_bull(outer_bull_diameter, bull_active ? active_bull : dim_green);
  draw_bull(inner_bull_diameter, bull_eye_active ? active_bull_eye : dim_red);
}

bool is_available() { return g_hatidart != nullptr; }

void start_game() {
  if (g_hatidart != nullptr) g_hatidart->start_game();
}

void HATiDart::setup() {
  g_hatidart = this;
  this->load_state();
  ESP_LOGI(TAG, "HATiDart scoring component initialized");
}

void HATiDart::loop() {}

void HATiDart::attach_click_sound(lv_obj_t *button) {
  if (button == nullptr || this->speaker_ == nullptr) return;
  lv_obj_add_event_cb(button, [](lv_event_t *event) {
    auto *component = static_cast<HATiDart *>(lv_event_get_user_data(event));
    if (component != nullptr) component->play_click();
  }, LV_EVENT_PRESSED, this);
}

void HATiDart::play_click() {
  if (this->speaker_ == nullptr) return;
  static std::array<int16_t, 960> samples{};
  static bool initialized = false;
  if (!initialized) {
    for (size_t index = 0; index < samples.size(); ++index) {
      const float phase = static_cast<float>(index % 54) / 54.0f;
      samples[index] = phase < 0.5f ? 9000 : -9000;
    }
    initialized = true;
  }
  this->speaker_->play(reinterpret_cast<const uint8_t *>(samples.data()),
                       samples.size() * sizeof(samples[0]));
}

void HATiDart::play_winning_sound() {
  if (this->speaker_ == nullptr) return;
  static std::array<int16_t, 3840> samples{};
  static bool initialized = false;
  if (!initialized) {
    constexpr int sample_rate = 16000;
    constexpr int note_samples = 1280;
    constexpr int frequencies[] = {660, 880, 1047};
    for (size_t index = 0; index < samples.size(); ++index) {
      const int note = static_cast<int>(index / note_samples);
      const int frequency = frequencies[note < 3 ? note : 2];
      const int period = sample_rate / frequency;
      const int position = static_cast<int>(index % note_samples);
      const int phase = position % period;
      samples[index] = phase < period / 2 ? 10000 : -10000;
    }
    initialized = true;
  }
  this->speaker_->play(reinterpret_cast<const uint8_t *>(samples.data()),
                       samples.size() * sizeof(samples[0]));
}

void HATiDart::dump_config() {
  ESP_LOGCONFIG(TAG, "HATiDart:");
  ESP_LOGCONFIG(TAG, "  Game: 501, up to 6 players, optional double-out");
  ESP_LOGCONFIG(TAG, "  State file: %s", this->state_file_.c_str());
}

float HATiDart::get_setup_priority() const { return esphome::setup_priority::DATA; }

void HATiDart::build_ui() {
  lv_obj_t *screen = lv_scr_act();
  if (screen == nullptr) return;
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  this->button_contexts_.clear();
  this->multiplier_buttons_.clear();
  this->button_contexts_.reserve(64);
  this->ui_root_ = lv_obj_create(screen);
  lv_obj_set_size(this->ui_root_, LV_PCT(100), LV_PCT(100));
  lv_obj_center(this->ui_root_);
  lv_obj_set_style_bg_color(this->ui_root_, lv_color_make(0x0E, 0x12, 0x18), 0);
  lv_obj_set_style_bg_opa(this->ui_root_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(this->ui_root_, 0, 0);
  lv_obj_set_style_pad_all(this->ui_root_, 8, 0);
  lv_obj_clear_flag(this->ui_root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(this->ui_root_, LV_SCROLLBAR_MODE_OFF);

  this->score_label_ = lv_label_create(this->ui_root_);
  lv_obj_set_style_text_font(this->score_label_, ui_font_for_size(32), 0);
  lv_obj_set_style_text_color(this->score_label_, lv_color_white(), 0);
  lv_obj_align(this->score_label_, LV_ALIGN_TOP_LEFT, 8, 4);
  this->player_scores_label_ = lv_label_create(this->ui_root_);
  lv_obj_set_style_text_font(this->player_scores_label_, ui_font_for_size(26), 0);
  lv_obj_set_style_text_color(this->player_scores_label_, lv_color_white(), 0);
  lv_obj_align(this->player_scores_label_, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  this->status_label_ = lv_label_create(this->ui_root_);
  lv_obj_set_style_text_font(this->status_label_, ui_font_for_size(28), 0);
  lv_obj_set_style_text_color(this->status_label_, lv_color_make(0x9E, 0xAC, 0xBD), 0);
  lv_obj_align(this->status_label_, LV_ALIGN_TOP_LEFT, 8, 130);
  this->darts_label_ = lv_label_create(this->ui_root_);
  lv_obj_set_style_text_font(this->darts_label_, ui_font_for_size(28), 0);
  lv_obj_set_style_text_color(this->darts_label_, lv_color_make(0xE8, 0xD0, 0x86), 0);
  lv_obj_align(this->darts_label_, LV_ALIGN_TOP_LEFT, 8, 166);

  lv_display_t *display = lv_display_get_default();
  const lv_coord_t screen_width =
      display == nullptr ? 800 : lv_display_get_horizontal_resolution(display);
  const lv_coord_t screen_height =
      display == nullptr ? 480 : lv_display_get_vertical_resolution(display);
  const lv_coord_t side_width = 156;
  const lv_coord_t board_size = std::max<lv_coord_t>(
      220, std::min<lv_coord_t>(screen_height, screen_width - side_width - 28));
  const lv_coord_t board_x = std::max<lv_coord_t>(
      8, (screen_width - side_width - board_size) / 2);
  const lv_coord_t board_y = 0;

  lv_obj_t *board = lv_obj_create(this->ui_root_);
  lv_obj_set_size(board, board_size, board_size);
  lv_obj_set_pos(board, board_x, board_y);
  lv_obj_set_style_bg_color(board, lv_color_make(0x16, 0x16, 0x16), 0);
  lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(board, 2, 0);
  lv_obj_set_style_border_color(board, lv_color_make(0xA0, 0xA0, 0xA0), 0);
  lv_obj_set_style_radius(board, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(board, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_shadow_width(board, 18, 0);
  lv_obj_set_style_shadow_color(board, lv_color_make(0x00, 0x00, 0x00), 0);
  lv_obj_set_style_shadow_opa(board, LV_OPA_50, 0);

  lv_obj_t *segments = lv_obj_create(board);
  this->segments_ = segments;
  lv_obj_set_size(segments, LV_PCT(100), LV_PCT(100));
  lv_obj_center(segments);
  lv_obj_set_style_bg_opa(segments, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(segments, 0, 0);
  lv_obj_clear_flag(segments, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(segments, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(segments, draw_dartboard_segments, LV_EVENT_DRAW_MAIN, nullptr);

  auto ring = [board](lv_coord_t size, lv_color_t color) {
    lv_obj_t *object = lv_obj_create(board);
    lv_obj_set_size(object, size, size);
    lv_obj_center(object);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
  };
  ring(static_cast<lv_coord_t>(board_size * 31.8f / 451.0f),
       lv_color_make(0x28, 0x8A, 0x55));
  ring(static_cast<lv_coord_t>(board_size * 12.7f / 451.0f),
       lv_color_make(0xC8, 0x32, 0x32));
  lv_obj_move_foreground(segments);

  // Segment buttons are arranged around the board so the dartboard remains
  // the primary visual and the hit targets stay easy to reach.
  const float pi = 3.14159265358979323846f;
  const lv_coord_t button_size = std::max<lv_coord_t>(80, board_size / 13);
  const lv_coord_t segment_radius = board_size * 43 / 100;
  static constexpr int board_numbers[] = {
      20, 1, 18, 4, 13, 6, 10, 15, 2, 17,
      3, 19, 7, 16, 8, 11, 14, 9, 12, 5,
  };
  for (size_t index = 0; index < sizeof(board_numbers) / sizeof(board_numbers[0]); ++index) {
    const int value = board_numbers[index];
    const float angle = (-90.0f + index * 18.0f) * pi / 180.0f;
    lv_obj_t *button = lv_button_create(board);
    this->attach_click_sound(button);
    lv_obj_set_size(button, button_size, button_size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_make(0x10, 0x14, 0x1A), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_color(button, lv_color_make(0xD1, 0xA9, 0x42), LV_STATE_PRESSED);
    lv_obj_align(
        button, LV_ALIGN_CENTER,
        static_cast<lv_coord_t>(std::cos(angle) * segment_radius),
        static_cast<lv_coord_t>(std::sin(angle) * segment_radius));
    lv_obj_t *label = lv_label_create(button);
    char text[8];
    snprintf(text, sizeof(text), "%d", value);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_font_for_size(56), 0);
    lv_obj_center(label);
    this->button_contexts_.push_back({this, value, ACTION_DART_VALUE});
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
      auto *context = static_cast<ButtonContext *>(lv_event_get_user_data(event));
      if (context != nullptr) context->component->select_dart_value(context->value);
    }, LV_EVENT_CLICKED, &this->button_contexts_.back());
  }

  const lv_coord_t bull_size = board_size * 14 / 100;
  lv_obj_t *bull_button = lv_button_create(board);
  this->attach_click_sound(bull_button);
  lv_obj_set_size(bull_button, bull_size, bull_size);
  lv_obj_set_style_radius(bull_button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(bull_button, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(bull_button, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(bull_button, lv_color_white(), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(bull_button, 3, 0);
  lv_obj_set_style_border_color(bull_button, lv_color_make(0x08, 0x08, 0x08), 0);
  lv_obj_set_style_border_opa(bull_button, LV_OPA_COVER, 0);
  lv_obj_align(bull_button, LV_ALIGN_CENTER, 0, 0);
  this->bull_label_ = lv_label_create(bull_button);
  lv_label_set_text(this->bull_label_, "25");
  lv_obj_set_style_text_color(this->bull_label_, lv_color_white(), 0);
  lv_obj_set_style_text_font(this->bull_label_, ui_font_for_size(32), 0);
  lv_obj_center(this->bull_label_);
  this->button_contexts_.push_back({this, 25, ACTION_DART_VALUE});
  lv_obj_add_event_cb(bull_button, [](lv_event_t *event) {
    auto *context = static_cast<ButtonContext *>(lv_event_get_user_data(event));
    if (context != nullptr) context->component->select_dart_value(context->value);
  }, LV_EVENT_CLICKED, &this->button_contexts_.back());

  lv_obj_t *controls = lv_obj_create(this->ui_root_);
  const lv_coord_t controls_x = board_x + board_size + 12;
  lv_obj_set_pos(controls, controls_x, board_y);
  lv_obj_set_size(controls, screen_width - controls_x - 8, board_size);
  lv_obj_set_style_pad_all(controls, 6, 0);
  lv_obj_set_style_bg_color(controls, lv_color_make(0x1A, 0x22, 0x2E), 0);
  lv_obj_set_style_bg_opa(controls, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(controls, 1, 0);
  lv_obj_set_style_border_color(controls, lv_color_make(0x35, 0x45, 0x58), 0);
  lv_obj_set_style_radius(controls, 12, 0);
  lv_obj_add_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(controls, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(controls, 2, 0);
  lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *multiplier_row = lv_obj_create(controls);
  lv_obj_set_width(multiplier_row, LV_PCT(100));
  lv_obj_set_height(multiplier_row, 84);
  lv_obj_set_style_bg_opa(multiplier_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(multiplier_row, 0, 0);
  lv_obj_set_style_pad_all(multiplier_row, 0, 0);
  lv_obj_set_flex_flow(multiplier_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(multiplier_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  for (int multiplier = 1; multiplier <= 3; ++multiplier) {
    lv_obj_t *button = lv_button_create(multiplier_row);
    this->attach_click_sound(button);
    lv_obj_set_width(button, LV_PCT(31));
    lv_obj_set_height(button, 84);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(button, lv_color_make(0x16, 0x66, 0xA8), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(button, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(button, lv_color_make(0xF0, 0xC9, 0x5B), LV_STATE_CHECKED);
    lv_obj_t *label = lv_label_create(button);
    char text[8];
    snprintf(text, sizeof(text), "x%d", multiplier);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_font_for_size(56), 0);
    lv_obj_center(label);
    this->button_contexts_.push_back({this, multiplier, ACTION_MULTIPLIER});
    this->multiplier_buttons_.push_back(button);
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
      auto *context = static_cast<ButtonContext *>(lv_event_get_user_data(event));
      if (context != nullptr) context->component->select_multiplier(context->value);
    }, LV_EVENT_CLICKED, &this->button_contexts_.back());
  }

  lv_obj_t *double_out_button = lv_button_create(controls);
  this->attach_click_sound(double_out_button);
  lv_obj_set_width(double_out_button, LV_PCT(100));
  lv_obj_set_height(double_out_button, 72);
  lv_obj_add_flag(double_out_button, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_set_style_bg_color(double_out_button, lv_color_make(0x16, 0x66, 0xA8), LV_STATE_CHECKED);
  lv_obj_set_style_border_width(double_out_button, 2, LV_STATE_CHECKED);
  lv_obj_set_style_border_color(double_out_button, lv_color_make(0xF0, 0xC9, 0x5B), LV_STATE_CHECKED);
  lv_obj_t *double_out_label = lv_label_create(double_out_button);
  lv_label_set_text(double_out_label, "Double out");
  lv_obj_set_style_text_color(double_out_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(double_out_label, ui_font_for_size(24), 0);
  lv_obj_center(double_out_label);
  this->button_contexts_.push_back({this, 0, ACTION_DOUBLE_OUT});
  lv_obj_add_event_cb(double_out_button, [](lv_event_t *event) {
    auto *context = static_cast<ButtonContext *>(lv_event_get_user_data(event));
    if (context != nullptr) {
      context->component->set_double_out(
          lv_obj_has_state(static_cast<lv_obj_t *>(lv_event_get_current_target(event)),
                           LV_STATE_CHECKED));
    }
  }, LV_EVENT_CLICKED, &this->button_contexts_.back());

  const struct {
    const char *text;
    int action;
  } action_buttons[] = {{"Submit score", ACTION_SUBMIT},
                        {"Correction", ACTION_CORRECTION},
                        {"Fail (0)", ACTION_FAIL},
                        {"New game", ACTION_RESET},
                        {"Close", ACTION_CLOSE}};
  for (const auto &definition : action_buttons) {
    if (definition.action == ACTION_RESET) {
      lv_obj_t *spacer = lv_obj_create(controls);
      lv_obj_set_width(spacer, LV_PCT(100));
      lv_obj_set_height(spacer, 1);
      lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(spacer, 0, 0);
      lv_obj_set_flex_grow(spacer, 1);
    }
    lv_obj_t *button = lv_button_create(controls);
    this->attach_click_sound(button);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, 100);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, definition.text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_font_for_size(32), 0);
    lv_obj_center(label);
    this->button_contexts_.push_back({this, 0, definition.action});
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
      auto *context = static_cast<ButtonContext *>(lv_event_get_user_data(event));
      if (context == nullptr) return;
      if (context->action == ACTION_SUBMIT) context->component->submit_visit();
      if (context->action == ACTION_FAIL) context->component->fail_visit();
      if (context->action == ACTION_CORRECTION) {
        if (context->component->dart_count_ > 0) {
          --context->component->dart_count_;
          context->component->darts_[context->component->dart_count_] = 0;
          context->component->doubles_[context->component->dart_count_] = false;
          context->component->save_state();
          context->component->status_text_ = "Correction applied";
          context->component->update_ui();
        }
      }
      if (context->action == ACTION_RESET) context->component->reset_game();
      if (context->action == ACTION_PLAYER_COUNT) context->component->start_new_game(context->value);
      if (context->action == ACTION_CLOSE) context->component->close_game();
    }, LV_EVENT_CLICKED, &this->button_contexts_.back());
  }

  this->ui_ready_ = true;
  if (this->double_out_) lv_obj_add_state(double_out_button, LV_STATE_CHECKED);
  if (this->selected_multiplier_ == 0) {
    this->clear_multiplier_selection();
  } else {
    this->select_multiplier(this->selected_multiplier_);
  }
  this->update_ui();
}

void HATiDart::start_game() {
  if (!this->ui_ready_) {
    this->build_ui();
  } else {
    lv_obj_clear_flag(this->ui_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->ui_root_);
    this->update_ui();
  }
}

void HATiDart::close_game() {
  if (this->ui_root_ != nullptr) {
    lv_obj_del(this->ui_root_);
    this->ui_root_ = nullptr;
  }
  this->score_label_ = nullptr;
  this->player_scores_label_ = nullptr;
  this->status_label_ = nullptr;
  this->darts_label_ = nullptr;
  this->bull_label_ = nullptr;
  this->segments_ = nullptr;
  this->new_game_dialog_ = nullptr;
  this->ui_ready_ = false;
  this->button_contexts_.clear();
  this->multiplier_buttons_.clear();
}

void HATiDart::update_ui() {
  if (!this->ui_ready_) return;
  char score[32];
  snprintf(score, sizeof(score), "P%d Score: %d", this->current_player_ + 1, this->score_);
  lv_label_set_text(this->score_label_, score);

  std::string player_scores;
  for (size_t index = 0; index < this->player_count_; ++index) {
    char player_score[32];
    snprintf(player_score, sizeof(player_score), "%sP%d: %d\n",
             index == this->current_player_ ? "> " : "  ",
             index + 1, this->player_scores_[index]);
    player_scores += player_score;
  }
  lv_label_set_text(this->player_scores_label_, player_scores.c_str());

  std::string darts = "Darts:";
  for (size_t index = 0; index < this->dart_count_; ++index) {
    char value[16];
    snprintf(value, sizeof(value), " %d", this->darts_[index]);
    darts += value;
  }
  lv_label_set_text(this->darts_label_, darts.c_str());
  lv_label_set_text(this->status_label_, this->status_text_.c_str());
}

void HATiDart::select_multiplier(int multiplier) {
  if (multiplier < 1 || multiplier > 3 || this->game_over_) return;
  this->selected_multiplier_ = multiplier;
  if (this->bull_label_ != nullptr) {
    lv_label_set_text(this->bull_label_, multiplier == 2 ? "50" : multiplier == 1 ? "25" : "--");
  }

  for (size_t index = 0; index < this->multiplier_buttons_.size(); ++index) {
    if (this->multiplier_buttons_[index] == nullptr) continue;
    if (static_cast<int>(index + 1) == multiplier) {
      lv_obj_add_state(this->multiplier_buttons_[index], LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(this->multiplier_buttons_[index], LV_STATE_CHECKED);
    }
  }
  if (this->segments_ != nullptr) lv_obj_invalidate(this->segments_);
  if (this->ui_ready_) {
    char status[32];
    snprintf(status, sizeof(status), "Multiplier: x%d", multiplier);
    this->status_text_ = status;
    lv_label_set_text(this->status_label_, this->status_text_.c_str());
  }
}

void HATiDart::clear_multiplier_selection() {
  this->selected_multiplier_ = 0;
  for (lv_obj_t *button : this->multiplier_buttons_) {
    if (button != nullptr) lv_obj_clear_state(button, LV_STATE_CHECKED);
  }
  if (this->bull_label_ != nullptr) lv_label_set_text(this->bull_label_, "--");
  if (this->segments_ != nullptr) lv_obj_invalidate(this->segments_);
}

void HATiDart::set_double_out(bool enabled) {
  this->double_out_ = enabled;
  this->status_text_ = enabled ? "Double out enabled" : "Double out disabled";
  this->save_state();
  this->update_ui();
}

void HATiDart::select_dart_value(int value) {
  if (value < 0 || value > 50 || this->game_over_ || this->dart_count_ >= this->darts_.size()) {
    return;
  }
  if (this->selected_multiplier_ == 0) {
    this->status_text_ = "Select x1, x2 or x3 first";
    this->update_ui();
    return;
  }
  if (value == 25 && this->selected_multiplier_ == 3) {
    this->status_text_ = "Bull only supports x1 or x2";
    this->update_ui();
    return;
  }
  const int dart_score = value == 50 ? 50 : value * this->selected_multiplier_;
  this->darts_[this->dart_count_] = dart_score;
  this->doubles_[this->dart_count_] =
      value == 50 || this->selected_multiplier_ == 2;
  ++this->dart_count_;
  this->selected_multiplier_ = 1;
  this->select_multiplier(1);
  this->save_state();
  this->update_ui();
}

void HATiDart::submit_visit() {
  if (this->game_over_ || this->dart_count_ == 0) {
    this->status_text_ = "Enter at least one dart first";
    if (this->ui_ready_) lv_label_set_text(this->status_label_, this->status_text_.c_str());
    return;
  }

  int total = 0;
  for (const int dart : this->darts_) total += dart;
  const bool checkout = total == this->score_;
  const bool final_dart_is_double = this->doubles_[this->dart_count_ - 1];
  if (total > this->score_ || (this->score_ - total == 1) ||
      (checkout && this->double_out_ && !final_dart_is_double)) {
    this->status_text_ = "Bust - visit not counted";
  } else {
    this->score_ -= total;
    this->player_scores_[this->current_player_] = this->score_;
    this->game_over_ = this->score_ == 0;
    this->save_state();
    this->status_text_ = this->game_over_ ? "Checkout!" : "Visit counted";
    if (this->game_over_) this->play_winning_sound();
  }

  this->dart_count_ = 0;
  this->darts_.fill(0);
  this->doubles_.fill(false);
  if (!this->game_over_ && this->player_count_ > 1) {
    this->current_player_ = (this->current_player_ + 1) % this->player_count_;
    this->score_ = this->player_scores_[this->current_player_];
    this->status_text_ = "Player " + std::to_string(this->current_player_ + 1) + "'s turn";
    this->save_state();
  }
  this->update_ui();
}

void HATiDart::fail_visit() {
  if (this->game_over_) return;
  this->dart_count_ = 0;
  this->darts_.fill(0);
  this->doubles_.fill(false);
  this->selected_multiplier_ = 0;
  this->select_multiplier(1);
  if (this->player_count_ > 1) {
    this->current_player_ = (this->current_player_ + 1) % this->player_count_;
    this->score_ = this->player_scores_[this->current_player_];
    this->save_state();
  }
  this->status_text_ = "Fail - 0 points";
  this->save_state();
  this->update_ui();
}

void HATiDart::show_new_game_dialog() {
  if (this->new_game_dialog_ != nullptr) return;
  this->new_game_dialog_ = lv_obj_create(this->ui_root_);
  lv_obj_set_size(this->new_game_dialog_, 360, 190);
  lv_obj_center(this->new_game_dialog_);
  lv_obj_set_style_bg_color(this->new_game_dialog_, lv_color_make(0x18, 0x22, 0x30), 0);
  lv_obj_set_style_bg_opa(this->new_game_dialog_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(this->new_game_dialog_, 2, 0);
  lv_obj_set_style_border_color(this->new_game_dialog_, lv_color_make(0x80, 0xA0, 0xC0), 0);
  lv_obj_set_style_radius(this->new_game_dialog_, 12, 0);
  lv_obj_set_style_pad_all(this->new_game_dialog_, 12, 0);
  lv_obj_clear_flag(this->new_game_dialog_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(this->new_game_dialog_, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *title = lv_label_create(this->new_game_dialog_);
  lv_label_set_text(title, "Number of players");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, ui_font_for_size(28), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

  lv_obj_t *row = lv_obj_create(this->new_game_dialog_);
  lv_obj_set_size(row, LV_PCT(100), 82);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  for (int players = 1; players <= 6; ++players) {
    lv_obj_t *button = lv_button_create(row);
    this->attach_click_sound(button);
    lv_obj_set_size(button, 48, 58);
    lv_obj_t *label = lv_label_create(button);
    char text[4];
    snprintf(text, sizeof(text), "%d", players);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_font_for_size(28), 0);
    lv_obj_center(label);
    this->button_contexts_.push_back({this, players, ACTION_PLAYER_COUNT});
    lv_obj_add_event_cb(button, [](lv_event_t *event) {
      auto *context = static_cast<ButtonContext *>(lv_event_get_user_data(event));
      if (context != nullptr) context->component->start_new_game(context->value);
    }, LV_EVENT_CLICKED, &this->button_contexts_.back());
  }
}

void HATiDart::start_new_game(int players) {
  if (players < 1 || players > 6) return;
  this->player_count_ = static_cast<uint8_t>(players);
  this->current_player_ = 0;
  this->player_scores_.fill(501);
  this->score_ = 501;
  this->selected_multiplier_ = 1;
  this->dart_count_ = 0;
  this->darts_.fill(0);
  this->doubles_.fill(false);
  this->game_over_ = false;
  this->double_out_ = true;
  this->status_text_ = players == 1 ? "New game" : "Player 1 starts";
  if (this->new_game_dialog_ != nullptr) {
    lv_obj_del(this->new_game_dialog_);
    this->new_game_dialog_ = nullptr;
  }
  this->clear_multiplier_selection();
  this->save_state();
  this->update_ui();
}

void HATiDart::reset_game() {
  this->show_new_game_dialog();
}

bool HATiDart::is_valid_state() const {
  if (this->score_ < 0 || this->score_ > 501 ||
      this->selected_multiplier_ < 0 || this->selected_multiplier_ > 3 ||
      this->player_count_ < 1 || this->player_count_ > 6 ||
      this->current_player_ >= this->player_count_ ||
      this->dart_count_ > this->darts_.size()) {
    return false;
  }
  for (size_t index = 0; index < this->dart_count_; ++index) {
    if (this->darts_[index] < 0 || this->darts_[index] > 60) return false;
  }
  return true;
}

bool HATiDart::load_state() {
  if (!esphome::spiffs::ensure_mounted()) return false;
  FILE *file = fopen(this->state_file_.c_str(), "rb");
  if (file == nullptr) return false;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  rewind(file);
  if (size <= 0 || size > 2048) {
    fclose(file);
    ESP_LOGW(TAG, "Saved game state file has an invalid size");
    return false;
  }
  std::string raw(static_cast<size_t>(size), '\0');
  if (fread(raw.data(), 1, raw.size(), file) != raw.size()) {
    fclose(file);
    ESP_LOGW(TAG, "Unable to read saved game state");
    return false;
  }
  fclose(file);
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, raw);
  if (error || document["version"] != 1) {
    ESP_LOGW(TAG, "Ignoring invalid saved game state");
    return false;
  }
  this->score_ = document["score"] | 501;
  this->player_count_ = document["player_count"] | 1;
  this->current_player_ = document["current_player"] | 0;
  for (size_t index = 0; index < this->player_scores_.size(); ++index) {
    this->player_scores_[index] = document["player_scores"][index] | 501;
  }
  if (this->player_count_ == 1) this->player_scores_[0] = this->score_;
  this->game_over_ = document["game_over"] | false;
  this->double_out_ = document["double_out"] | true;
  this->selected_multiplier_ = document["selected_multiplier"] | 1;
  this->dart_count_ = document["dart_count"] | 0;
  for (size_t index = 0; index < this->darts_.size(); ++index) {
    this->darts_[index] = document["darts"][index] | 0;
    this->doubles_[index] = document["doubles"][index] | false;
  }
  if (!this->is_valid_state()) {
    this->score_ = 501;
    this->player_count_ = 1;
    this->current_player_ = 0;
    this->player_scores_.fill(501);
    this->selected_multiplier_ = 1;
    this->dart_count_ = 0;
    this->darts_.fill(0);
    this->doubles_.fill(false);
    this->game_over_ = false;
    ESP_LOGW(TAG, "Saved game state is out of range; starting a new game");
    return false;
  }
  ESP_LOGI(TAG, "Restored saved game with score %d", this->score_);
  return true;
}

bool HATiDart::save_state() const {
  if (!esphome::spiffs::ensure_mounted()) return false;
  FILE *file = fopen(this->state_file_.c_str(), "wb");
  if (file == nullptr) {
    ESP_LOGW(TAG, "Unable to open %s for writing", this->state_file_.c_str());
    return false;
  }
  JsonDocument document;
  document["version"] = 1;
  document["score"] = this->score_;
  document["player_count"] = this->player_count_;
  document["current_player"] = this->current_player_;
  JsonArray player_scores = document["player_scores"].to<JsonArray>();
  for (const int player_score : this->player_scores_) player_scores.add(player_score);
  document["game_over"] = this->game_over_;
  document["double_out"] = this->double_out_;
  document["selected_multiplier"] = this->selected_multiplier_;
  document["dart_count"] = this->dart_count_;
  JsonArray darts = document["darts"].to<JsonArray>();
  JsonArray doubles = document["doubles"].to<JsonArray>();
  for (size_t index = 0; index < this->darts_.size(); ++index) {
    darts.add(this->darts_[index]);
    doubles.add(this->doubles_[index]);
  }
  std::string raw;
  serializeJson(document, raw);
  const bool success = fwrite(raw.data(), 1, raw.size(), file) == raw.size();
  fclose(file);
  return success;
}

}  // namespace hatidart
