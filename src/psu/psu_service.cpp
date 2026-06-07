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

/* Serializes all high-level PsuController access: the 500 ms poll, the LVGL
 * UI callbacks, and the UDP command server (CONFIG_APP_WIFI_AP). */
K_MUTEX_DEFINE(g_cmd_mutex);

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

  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  Controller().ReadMeasurement(&g_measurement);

  const int64_t elapsed_ms = k_uptime_delta(&g_last_sample_ms);
  const float power_w = g_measurement.volts * g_measurement.amps;
  g_energy_wh += power_w * elapsed_ms / 1000.0f / 3600.0f;
  k_mutex_unlock(&g_cmd_mutex);

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
  if (psu_cmd_toggle_output(&enabled) == 0) {
    lv_label_set_text(ui_LabelOnOff, enabled ? "ON" : "OFF");
  }
}

extern "C" void psu_mode_toggle_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  bool now_cc;
  if (psu_cmd_toggle_mode(&now_cc) == 0) {
    lv_label_set_text(ui_LabelCVCC, now_cc ? "CC" : "CV");
  }
}

extern "C" void psu_current_set_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  struct psu_status st;
  if (psu_cmd_get_status(&st) != 0) {
    return;
  }
  float current = st.cc_amps;

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
  psu_cmd_set_current(current);
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
  return psu_cmd_set_fan(rpm);
}

extern "C" int psu_cmd_toggle_output(bool* now_on) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  bool enabled;
  int rc = Controller().ToggleOutput(&enabled) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  if (rc == 0 && now_on) {
    *now_on = enabled;
  }
  return rc;
}

extern "C" int psu_cmd_set_output(bool on) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  bool cur;
  int rc = -1;
  if (Controller().GetOutputEnabled(&cur)) {
    if (cur == on) {
      rc = 0;
    } else {
      bool now;
      rc = (Controller().ToggleOutput(&now) && now == on) ? 0 : -1;
    }
  }
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_toggle_mode(bool* now_cc) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  psu::OutputMode mode;
  int rc = Controller().ToggleMode(&mode) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  if (rc == 0 && now_cc) {
    *now_cc = (mode == psu::OutputMode::kConstantCurrent);
  }
  return rc;
}

extern "C" int psu_cmd_set_mode(bool cc) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  psu::OutputMode mode;
  int rc = -1;
  if (Controller().GetMode(&mode)) {
    bool is_cc = (mode == psu::OutputMode::kConstantCurrent);
    if (is_cc == cc) {
      rc = 0;
    } else {
      psu::OutputMode now;
      rc = (Controller().ToggleMode(&now) &&
            (now == psu::OutputMode::kConstantCurrent) == cc) ? 0 : -1;
    }
  }
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_set_current(float amps) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  int rc = Controller().SetConstantCurrent(amps) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_set_fan(int rpm) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  int rc = Controller().SetFanRpm(rpm) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_get_status(struct psu_status* out) {
  if (out == nullptr) {
    return -1;
  }
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  out->volts = g_measurement.volts;
  out->amps = g_measurement.amps;
  out->watts = g_measurement.watts;
  out->energy_wh = g_energy_wh;

  bool enabled = false;
  psu::OutputMode mode = psu::OutputMode::kConstantVoltage;
  float cc = 0.0f;
  bool ok = Controller().GetOutputEnabled(&enabled) &&
            Controller().GetMode(&mode) &&
            Controller().GetConstantCurrent(&cc);
  out->output_on = enabled;
  out->mode_cc = (mode == psu::OutputMode::kConstantCurrent);
  out->cc_amps = cc;
  k_mutex_unlock(&g_cmd_mutex);
  return ok ? 0 : -1;
}

extern "C" int psu_service_init(void) {
  g_last_sample_ms = k_uptime_get();
  (void)Controller();  // force construction now
  k_work_schedule(&g_poll_work, K_MSEC(kInitialPollDelayMs));
  return 0;
}

SYS_INIT(psu_service_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
