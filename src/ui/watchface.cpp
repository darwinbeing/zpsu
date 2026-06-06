#include "watchface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <lvgl.h>
#include <zephyr/logging/log.h>

#include "ui.h"
#include "psu_service.h"
#include "splash.hpp"

LOG_MODULE_REGISTER(watchface_ui, LOG_LEVEL_WRN);

namespace {

int format_val(float num, char* buf) {
  if (num <= 0) {
    sprintf(buf, "%s", "00.00");
  } else if (0 < num && num < 1) {
    sprintf(buf, "%.3f", num);
  } else {
    sprintf(buf, "%.*f",
            std::max(0, 3 - static_cast<int>(floor(log10(fabs(num))))), num);
  }
  return 0;
}

}  // namespace

namespace ui {

void Watchface::Init() {}

void Watchface::Show() {
  ui_init();
  psu_ui_init();
  Splash::Show();
}

void Watchface::Remove() {}
void Watchface::SetBatteryPercent(int32_t percent, int32_t value) {}
void Watchface::SetHrm(int32_t value) {}
void Watchface::SetStep(int32_t value) {}
void Watchface::SetTime(int32_t hour, int32_t minute, int32_t second) {}
void Watchface::SetBleConnected(bool connected) {}
void Watchface::SetNumNotifications(int32_t value) {}
void Watchface::SetWeather(int8_t temperature, int weather_code) {}
void Watchface::SetDate(int day_of_week, int date) {}

void Watchface::SetEnergyPanel(const struct psuctrl_data_event* evt) {
  char buf[32];

  LOG_DBG("PSU: %f %f %f %f %d", evt->volts, evt->amps, evt->watts, evt->energy,
          evt->is_kWh);

  format_val(evt->volts, buf);
  lv_label_set_text(ui_LabelVoltage, buf);

  format_val(evt->amps, buf);
  lv_label_set_text(ui_LabelCurrent, buf);

  format_val(evt->watts, buf);
  lv_label_set_text(ui_LabelPower, buf);

  format_val(evt->energy, buf);
  if (evt->is_kWh) {
    lv_label_set_text(ui_LabelEnergy, buf);
    lv_label_set_text(ui_Label8, "kWh");
  } else {
    lv_label_set_text(ui_LabelEnergy, buf);
    lv_label_set_text(ui_Label8, "Wh");
  }
}

}  // namespace ui
