#ifndef APP_APP_HPP_
#define APP_APP_HPP_

#include "singleton.hpp"

namespace app {

// Top-level application: owns the boot/init sequence.
class App : public common::Singleton<App> {
  friend class common::Singleton<App>;

 public:
  void Start();    // submit init work to the system workqueue (called from main)
  // Internal: workqueue entry point invoked by RunInitWork() in app.cpp.
  void RunInit();  // runs on the system workqueue

 private:
  App() = default;
  void InitRgbPwmLed();
};

}  // namespace app

#endif  // APP_APP_HPP_
