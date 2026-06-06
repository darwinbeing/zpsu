#ifndef DISPLAY_DISPLAY_CONTROL_HPP_
#define DISPLAY_DISPLAY_CONTROL_HPP_

#include <cstdint>

#include "singleton.hpp"

namespace display {

class DisplayControl : public common::Singleton<DisplayControl> {
  friend class common::Singleton<DisplayControl>;

 public:
  void Init();
  void PowerOn(bool on);
  void SetBrightness(uint8_t percent);
  void LvglUpdate();

 private:
  DisplayControl() = default;

  bool is_on_ = false;
  uint8_t last_brightness_ = 80;
};

}  // namespace display

#endif  // DISPLAY_DISPLAY_CONTROL_HPP_
