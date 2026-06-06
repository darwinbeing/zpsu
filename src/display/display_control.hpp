#ifndef DISPLAY_DISPLAY_CONTROL_HPP_
#define DISPLAY_DISPLAY_CONTROL_HPP_

#include <cstdint>

namespace display {

class DisplayControl {
 public:
  static DisplayControl& Instance();

  void Init();
  void PowerOn(bool on);
  void SetBrightness(uint8_t percent);
  void LvglUpdate();

 private:
  DisplayControl() = default;
  DisplayControl(const DisplayControl&) = delete;
  DisplayControl& operator=(const DisplayControl&) = delete;
  DisplayControl(DisplayControl&&) = delete;
  DisplayControl& operator=(DisplayControl&&) = delete;

  bool is_on_ = false;
  uint8_t last_brightness_ = 80;
};

}  // namespace display

#endif  // DISPLAY_DISPLAY_CONTROL_HPP_
