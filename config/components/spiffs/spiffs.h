#pragma once

#include "esphome/core/component.h"

namespace esphome {
namespace spiffs {

class Spiffs : public Component {
 public:
  void setup() override;
  float get_setup_priority() const override;
};

bool ensure_mounted();
bool is_mounted();

}  // namespace spiffs
}  // namespace esphome
