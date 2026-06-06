#ifndef UI_WATCHFACE_HPP_
#define UI_WATCHFACE_HPP_

#include <cstdint>

#include <events/psuctrl_event.h>

namespace ui {

// LVGL watchface widget controller (stateless).
class Watchface {
 public:
  static void Init();
  static void Show();
  static void Remove();
  static void SetBatteryPercent(int32_t percent, int32_t value);
  static void SetHrm(int32_t value);
  static void SetStep(int32_t value);
  static void SetTime(int32_t hour, int32_t minute, int32_t second);
  static void SetBleConnected(bool connected);
  static void SetNumNotifications(int32_t value);
  static void SetWeather(int8_t temperature, int weather_code);
  static void SetDate(int day_of_week, int date);
  static void SetEnergyPanel(const struct psuctrl_data_event* event);
};

}  // namespace ui

#endif  // UI_WATCHFACE_HPP_
