#include "psu_service.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <cstdio>

#include <events/psuctrl_event.h>
#include "psu_controller.hpp"
#include "ui.h"

LOG_MODULE_REGISTER(psu_service, LOG_LEVEL_WRN);

ZBUS_CHAN_DECLARE(psuctrl_data_chan);

namespace {

constexpr int kPollIntervalMs = 500;
constexpr int kInitialPollDelayMs = 10;
constexpr float kDefaultCurrentAmps = 20.0f;
constexpr float kWhPerKwh = 1000.0f;

// Meyers singleton: constructed on first use, i.e. during psu_service_init()
// at APPLICATION init level when the I2C device is already ready.
psu::PsuController& Controller() {
  static psu::PsuController controller(DEVICE_DT_GET(DT_NODELABEL(i2c0)), 7);
  return controller;
}

// Persistent measurement state (last good values retained on read failure).
psu::Measurement g_measurement{};
float g_energy_wh = 0.0f;
int64_t g_last_sample_ms = 0;

void PollWork(struct k_work* work);
K_WORK_DELAYABLE_DEFINE(g_poll_work, PollWork);

void PollWork(struct k_work* work) {
  ARG_UNUSED(work);

  Controller().ReadMeasurement(&g_measurement);

  const int64_t elapsed_ms = k_uptime_delta(&g_last_sample_ms);
  const float power_w = g_measurement.volts * g_measurement.amps;
  g_energy_wh += power_w * elapsed_ms / 1000.0f / 3600.0f;

  struct psuctrl_data_event evt = {};
  evt.volts = g_measurement.volts;
  evt.amps = g_measurement.amps;
  evt.watts = g_measurement.watts;
  if (g_energy_wh >= kWhPerKwh) {
    evt.energy = g_energy_wh / kWhPerKwh;
    evt.is_kWh = 1;
  } else {
    evt.energy = g_energy_wh;
    evt.is_kWh = 0;
  }

  zbus_chan_pub(&psuctrl_data_chan, &evt, K_MSEC(250));
  k_work_schedule(&g_poll_work, K_MSEC(kPollIntervalMs));
}

}  // namespace

extern "C" void psu_power_toggle_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  bool enabled;
  if (Controller().ToggleOutput(&enabled)) {
    lv_label_set_text(ui_LabelOnOff, enabled ? "ON" : "OFF");
  }
}

extern "C" void psu_mode_toggle_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  psu::OutputMode mode;
  if (Controller().ToggleMode(&mode)) {
    lv_label_set_text(ui_LabelCVCC,
                      mode == psu::OutputMode::kConstantCurrent ? "CC" : "CV");
  }
}

extern "C" void psu_current_set_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  float current;
  if (!Controller().GetConstantCurrent(&current)) {
    return;
  }

  if (code == LV_EVENT_KEY) {
    const uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP) {
      current += 0.5f;
    } else if (key == LV_KEY_DOWN) {
      current -= 0.5f;
    }
  } else if (code == LV_EVENT_LONG_PRESSED ||
             code == LV_EVENT_LONG_PRESSED_REPEAT) {
    const uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP) {
      current = 5.0f;
    } else if (key == LV_KEY_DOWN) {
      current = -5.0f;
    }
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(current));
  Controller().SetConstantCurrent(current);
  lv_label_set_text(ui_lblPsuCc, buf);
}

extern "C" void psu_ui_init(void) {
  bool enabled;
  if (Controller().GetOutputEnabled(&enabled)) {
    lv_label_set_text(ui_LabelOnOff, enabled ? "ON" : "OFF");
  }
  psu::OutputMode mode;
  if (Controller().GetMode(&mode)) {
    lv_label_set_text(ui_LabelCVCC,
                      mode == psu::OutputMode::kConstantCurrent ? "CC" : "CV");
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(kDefaultCurrentAmps));
  if (Controller().SetConstantCurrent(kDefaultCurrentAmps)) {
    lv_label_set_text(ui_lblPsuCc, buf);
  }
}

extern "C" int psu_set_fan_rpm(int rpm) {
  return Controller().SetFanRpm(rpm) ? 0 : -1;
}

extern "C" int psu_service_init(void) {
  g_last_sample_ms = k_uptime_get();
  (void)Controller();  // force construction now
  k_work_schedule(&g_poll_work, K_MSEC(kInitialPollDelayMs));
  return 0;
}

SYS_INIT(psu_service_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
