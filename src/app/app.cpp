#include "app.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_APP_FLUSH_BENCH
#include <zephyr/drivers/display.h>
#include <zephyr/sys/printk.h>
#endif

#include "display_control.hpp"
#include "watchface_app.hpp"

LOG_MODULE_REGISTER(app, LOG_LEVEL_WRN);

#ifdef CONFIG_APP_FLUSH_BENCH
/* One-shot SPI flush benchmark, off by default (CONFIG_APP_FLUSH_BENCH).
 * Enable with `-DCONFIG_APP_FLUSH_BENCH=y` to re-measure DMA vs polling:
 * raw flush bandwidth + CPU left free during transfers. */
namespace {

void RunFlushBench() {
  const struct device* disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
  if (!device_is_ready(disp)) {
    printk("BENCH: display not ready\n");
    return;
  }

  struct display_capabilities cap;
  display_get_capabilities(disp, &cap);
  const uint16_t w = cap.x_resolution;
  const uint16_t h = cap.y_resolution;
  const uint16_t rows = 16;
  static uint16_t buf[320 * 16]; /* 10 KiB; assumes width <= 320 */
  for (size_t i = 0; i < (size_t)w * rows; i++) {
    buf[i] = 0xF800; /* value irrelevant to timing */
  }

  struct display_buffer_descriptor desc = {};
  desc.buf_size = (uint32_t)w * rows * 2;
  desc.width = w;
  desc.height = rows;
  desc.pitch = w;

  const int strips = h / rows;
  const int frames = 10;

  /* Accurate CPU utilisation straight from the scheduler's idle accounting
   * (CONFIG_THREAD_RUNTIME_STATS). For CPU stats, execution_cycles counts
   * non-idle + idle (elapsed) and total_cycles counts non-idle (busy), so
   * CPU-free = (elapsed - busy) / elapsed. No spinner, no baseline guesswork. */
  k_thread_runtime_stats_t rs0 = {}, rs1 = {};
  uint32_t t0 = k_cycle_get_32();
  k_thread_runtime_stats_cpu_get(0, &rs0);

  for (int f = 0; f < frames; f++) {
    for (int s = 0; s < strips; s++) {
      display_write(disp, 0, s * rows, &desc, buf);
    }
  }

  k_thread_runtime_stats_cpu_get(0, &rs1);
  uint32_t cyc = k_cycle_get_32() - t0;

  uint64_t us = k_cyc_to_ns_floor64(cyc) / 1000ULL;
  uint32_t bytes = (uint32_t)frames * strips * w * rows * 2;
  uint64_t kbs = us ? ((uint64_t)bytes * 1000000ULL / us) / 1024ULL : 0;
  uint64_t frame_bytes = (uint64_t)w * h * 2;
  uint64_t frame_us = bytes ? (us * frame_bytes / bytes) : 0;

  const uint64_t elapsed = rs1.execution_cycles - rs0.execution_cycles; /* non-idle + idle */
  const uint64_t busy = rs1.total_cycles - rs0.total_cycles;            /* non-idle */
  const uint32_t cpu_free_pct =
      elapsed ? (uint32_t)((elapsed - busy) * 100ULL / elapsed) : 0;

  printk("\n==== FLUSH BENCH ====\n");
  printk("xfer=%u bytes in %llu us -> %llu KB/s\n", bytes, us, kbs);
  printk("full-frame %ux%u = %llu.%llu ms (%llu fps cap by flush)\n", w, h,
         frame_us / 1000ULL, (frame_us % 1000ULL) / 100ULL,
         frame_us ? 1000000ULL / frame_us : 0);
  printk("CPU free during flush = %u%% (scheduler idle/elapsed)\n", cpu_free_pct);
  printk("=====================\n\n");
}

}  // namespace
#endif  // CONFIG_APP_FLUSH_BENCH

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

#ifdef CONFIG_APP_FLUSH_BENCH
  RunFlushBench();
#endif

  const struct device* lvgl_btn_dev =
      DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_button_input));
  if (!device_is_ready(lvgl_btn_dev)) {
    LOG_ERR("Device not ready, aborting...");
    return;
  }

  app::WatchfaceApp::Instance().Start(nullptr);
}

}  // namespace app
