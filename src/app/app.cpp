#include "app.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display_control.hpp"
#include "watchface_app.hpp"

LOG_MODULE_REGISTER(app, LOG_LEVEL_WRN);

namespace {

const struct pwm_dt_spec red_pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(red_pwm_led));
const struct pwm_dt_spec green_pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(green_pwm_led));
const struct pwm_dt_spec blue_pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(blue_pwm_led));

void RunInitWork(struct k_work* work) {
  ARG_UNUSED(work);
  app::App::Instance().RunInit();
}
K_WORK_DEFINE(init_work, RunInitWork);

}  // namespace

namespace app {

void App::Start() {
  // The init code needs stack; reuse the system workqueue for init instead of
  // growing CONFIG_MAIN_STACK_SIZE.
  k_work_submit(&init_work);
}

void App::InitRgbPwmLed() {
  uint32_t pulse_red = 0, pulse_green = 0, pulse_blue = 0;
  int ret;

  printk("PWM-based RGB LED control\n");

  if (!device_is_ready(red_pwm_led.dev) ||
      !device_is_ready(green_pwm_led.dev) ||
      !device_is_ready(blue_pwm_led.dev)) {
    printk("Error: one or more PWM devices not ready\n");
    return;
  }

  ret = pwm_set_pulse_dt(&red_pwm_led, pulse_red);
  if (ret != 0) {
    printk("Error %d: red write failed\n", ret);
    return;
  }
  ret = pwm_set_pulse_dt(&green_pwm_led, pulse_green);
  if (ret != 0) {
    printk("Error %d: green write failed\n", ret);
    return;
  }
  ret = pwm_set_pulse_dt(&blue_pwm_led, pulse_blue);
  if (ret != 0) {
    printk("Error %d: blue write failed\n", ret);
    return;
  }
}

void App::RunInit() {
  InitRgbPwmLed();
  // psu_service_init() is registered via SYS_INIT() in psu_service.cpp
  display::DisplayControl::Instance().Init();
  display::DisplayControl::Instance().PowerOn(true);

  const struct device* lvgl_btn_dev =
      DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_button_input));
  if (!device_is_ready(lvgl_btn_dev)) {
    LOG_ERR("Device not ready, aborting...");
    return;
  }

  app::WatchfaceApp::Instance().Start(nullptr);
}

}  // namespace app
