#pragma once

#include <array>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace hatidart {

bool is_available();
void start_game();

class HATiDart : public esphome::Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;
  void start_game();
  int selected_multiplier() const { return this->selected_multiplier_; }

  void set_state_file(const char *path) { this->state_file_ = path ? path : ""; }
  void set_speaker(esphome::speaker::Speaker *speaker) { this->speaker_ = speaker; }

 protected:
  struct ButtonContext {
    HATiDart *component;
    int value;
    int action;
  };

  void build_ui();
  void update_ui();
  void select_multiplier(int multiplier);
  void clear_multiplier_selection();
  void set_double_out(bool enabled);
  void show_new_game_dialog();
  void start_new_game(int players);
  void attach_click_sound(lv_obj_t *button);
  void play_click();
  void play_winning_sound();
  void select_dart_value(int value);
  void submit_visit();
  void fail_visit();
  void reset_game();
  void close_game();
  bool load_state();
  bool save_state() const;
  bool is_valid_state() const;
  int score_{501};
  std::array<int, 6> player_scores_{{501, 501, 501, 501, 501, 501}};
  uint8_t player_count_{1};
  uint8_t current_player_{0};
  int selected_multiplier_{1};
  size_t dart_count_{0};
  std::array<int, 3> darts_{};
  std::array<bool, 3> doubles_{};
  bool game_over_{false};
  bool double_out_{true};
  bool ui_ready_{false};
  std::string state_file_{"/spiffs/hatidart.json"};
  std::string status_text_{"Select a dart"};
  lv_obj_t *score_label_{nullptr};
  lv_obj_t *player_scores_label_{nullptr};
  lv_obj_t *status_label_{nullptr};
  lv_obj_t *darts_label_{nullptr};
  lv_obj_t *bull_label_{nullptr};
  lv_obj_t *ui_root_{nullptr};
  lv_obj_t *segments_{nullptr};
  lv_obj_t *new_game_dialog_{nullptr};
  esphome::speaker::Speaker *speaker_{nullptr};
  std::vector<ButtonContext> button_contexts_;
  std::vector<lv_obj_t *> multiplier_buttons_;
};

}  // namespace hatidart
