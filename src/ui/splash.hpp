#ifndef UI_SPLASH_HPP_
#define UI_SPLASH_HPP_

namespace ui {

// Boot splash on the top layer: a blue ring fills under "PSU MONITOR", then the
// overlay dissolves to reveal the watchface (~1.5s). Self-deleting; call once
// after the watchface screen exists.
class Splash {
 public:
  static void Show();
};

}  // namespace ui

#endif  // UI_SPLASH_HPP_
