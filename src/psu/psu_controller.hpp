#ifndef PSU_PSU_CONTROLLER_HPP_
#define PSU_PSU_CONTROLLER_HPP_

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <cstdint>

namespace psu {

struct Measurement {
  float volts;
  float amps;
  float watts;
};

enum class OutputMode { kConstantVoltage, kConstantCurrent };

// Drives a DPS1200-style PSU over I2C. Pure logic: no LVGL, no zbus.
class PsuController {
 public:
  // address_index is added to the DPS base address (0x58).
  PsuController(const device* i2c_dev, uint8_t address_index);

  bool IsReady() const;

  // DPS1200 register access (handles the two's-complement checksum framing).
  bool ReadRegister(uint8_t reg, int* value);
  bool WriteRegister(uint8_t reg, int value);

  // High-level operations. Each returns false on I2C/checksum failure and
  // leaves out-parameters unchanged.
  bool ReadMeasurement(Measurement* out);  // regs 7/8/9, updates fields read OK
  bool SetFanRpm(int rpm);
  bool GetOutputEnabled(bool* enabled);
  bool ToggleOutput(bool* enabled_out);
  bool GetMode(OutputMode* mode);
  bool ToggleMode(OutputMode* mode_out);
  bool GetConstantCurrent(float* amps_out);
  bool SetConstantCurrent(float amps);

 private:
  bool ReadFrame(uint8_t wire_reg, uint8_t* data, uint32_t count);

  const device* i2c_dev_;
  uint8_t device_address_;
  k_mutex bus_mutex_;
};

}  // namespace psu

#endif  // PSU_PSU_CONTROLLER_HPP_
