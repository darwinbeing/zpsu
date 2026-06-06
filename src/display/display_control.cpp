#include "display_control.hpp"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include "lvgl.h"

LOG_MODULE_REGISTER(display_control, LOG_LEVEL_WRN);

namespace {

const struct pwm_dt_spec display_blk = PWM_DT_SPEC_GET_OR(DT_ALIAS(display_blk), {});
const struct device* const reg_dev = DEVICE_DT_GET_OR_NULL(DT_PATH(regulator_3v3_ctrl));
const struct device* display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

void lvgl_render(struct k_work* item);
K_WORK_DELAYABLE_DEFINE(lvgl_work, lvgl_render);
struct k_work_sync canel_work_sync;

void lvgl_render(struct k_work* item) {
  const int64_t next_update_in_ms = lv_task_handler();
  k_work_schedule(&lvgl_work, K_MSEC(next_update_in_ms));
}

// Backlight is turned on only after the first frame has been painted, so the
// panel's uninitialized GRAM is never shown as garbage at power-up.
constexpr uint32_t kBacklightDelayMs = 150;

void backlight_on_work_cb(struct k_work* item);
K_WORK_DELAYABLE_DEFINE(backlight_on_work, backlight_on_work_cb);

void backlight_on_work_cb(struct k_work* item) {
  display::DisplayControl::Instance().ShowBacklight();
}

}  // namespace

namespace display {

void DisplayControl::Init() {
  if (!device_is_ready(display_dev)) {
    LOG_ERR("Device display not ready.");
    return;
  }
  if (!device_is_ready(display_blk.dev)) {
    LOG_WRN("Display brightness control not supported");
    return;
  }
  if (!device_is_ready(reg_dev)) {
    LOG_WRN("Display regulator control not supported");
    return;
  }
}

void DisplayControl::PowerOn(bool on) {
  if (on == is_on_) {
    return;
  }
  is_on_ = on;
  if (on) {
    if (device_is_ready(reg_dev)) {
      regulator_enable(reg_dev);
      pm_device_action_run(display_dev, PM_DEVICE_ACTION_TURN_ON);
    } else {
      pm_device_action_run(display_dev, PM_DEVICE_ACTION_RESUME);
    }
    display_blanking_off(display_dev);
    // Defer the backlight until the first frame is rendered (avoids showing
    // uninitialized panel GRAM as garbage on power-up).
    k_work_schedule(&backlight_on_work, K_MSEC(kBacklightDelayMs));
    k_work_schedule(&lvgl_work, K_MSEC(1));
  } else {
    if (device_is_ready(reg_dev)) {
      regulator_disable(reg_dev);
      pm_device_action_run(display_dev, PM_DEVICE_ACTION_TURN_OFF);
    } else {
      pm_device_action_run(display_dev, PM_DEVICE_ACTION_SUSPEND);
    }
    k_work_cancel_delayable(&backlight_on_work);
    SetBrightness(0);
    k_work_cancel_delayable_sync(&lvgl_work, &canel_work_sync);
    lv_obj_invalidate(lv_scr_act());
  }
}

void DisplayControl::SetBrightness(uint8_t percent) {
#define LED_PWM_PERIOD_US (1000U)
  if (!device_is_ready(display_blk.dev)) {
    return;
  }
  __ASSERT(percent >= 0 && percent <= 100,
           "Invalid range for brightness, valid range 0-100, was %d", percent);
  int ret;
  uint32_t step = display_blk.period / 100;
  uint32_t pulse_width = step * percent;

  last_brightness_ = percent;

  uint32_t period = PWM_USEC(LED_PWM_PERIOD_US);
  step = period / 100;
  pulse_width = step * percent;
  ret = pwm_set_dt(&display_blk, period, pulse_width);

  __ASSERT(ret == 0, "pwm error: %d for pulse: %d", ret, pulse_width);
}

void DisplayControl::LvglUpdate() {
  const int64_t next_update_in_ms = lv_task_handler();
  k_work_schedule(&lvgl_work, K_MSEC(next_update_in_ms));
}

void DisplayControl::ShowBacklight() { SetBrightness(last_brightness_); }

}  // namespace display
