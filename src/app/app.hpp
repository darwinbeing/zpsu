#ifndef APP_APP_HPP_
#define APP_APP_HPP_

namespace app {

// Top-level application: owns the boot/init sequence.
class App {
 public:
  static App& Instance();

  void Start();    // submit init work to the system workqueue (called from main)
  // Internal: workqueue entry point invoked by RunInitWork() in app.cpp.
  void RunInit();  // runs on the system workqueue

 private:
  App() = default;
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = delete;
  App& operator=(App&&) = delete;
  void InitRgbPwmLed();
};

}  // namespace app

#endif  // APP_APP_HPP_
