#include "led.hpp"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_app, LOG_LEVEL_DBG);

#define LED0_NODE DT_ALIAS(led0)

namespace {

/* GET_OR (not GET) so this builds on boards without an onboard led0 — e.g. the
 * Pico W, where GP25 belongs to the CYW43439 and rpi_pico-led.dtsi is omitted.
 * The empty fallback spec makes gpio_is_ready_dt() fail in Start(), so the
 * heartbeat LED is simply a no-op there. Mirrors display_blk in
 * display_control.cpp; byte-identical behaviour on the plain Pico. */
const struct gpio_dt_spec led_spec = GPIO_DT_SPEC_GET_OR(LED0_NODE, gpios, {});

void led_blink_timer_cb(struct k_timer* timer_id) {
  gpio_pin_toggle_dt(&led_spec);
}
K_TIMER_DEFINE(led_blink_timer, led_blink_timer_cb, NULL);

}  // namespace

namespace led {

void Led::Start() {
  int ret;

  if (!gpio_is_ready_dt(&led_spec)) {
    return;
  }

  ret = gpio_pin_configure_dt(&led_spec, GPIO_OUTPUT_ACTIVE);
  if (ret < 0) {
    return;
  }

  gpio_pin_set_dt(&led_spec, 0);
  k_timer_stop(&led_blink_timer);
  k_timer_start(&led_blink_timer, K_MSEC(1000), K_MSEC(1000));
}

void Led::Stop() {
  k_timer_stop(&led_blink_timer);
}

}  // namespace led

static int led_app_add(void) {
  led::Led::Instance().Start();
  return 0;
}
SYS_INIT(led_app_add, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
