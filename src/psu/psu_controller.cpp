#include "psu_controller.hpp"

#ifndef OVP_SCALE_FACTOR
#define OVP_SCALE_FACTOR 1.0
#endif

namespace psu {
namespace {

constexpr uint8_t kRegVolts = 7;
constexpr uint8_t kRegAmps = 8;
constexpr uint8_t kRegWatts = 9;
constexpr uint8_t kRegConstantCurrent = 0x1f;
constexpr uint8_t kRegStatus = 0x21;
constexpr uint8_t kRegFanRpm = 0x20;

constexpr float kCurrentScale = 265.0f;
constexpr float kAmpsScale = 64.0f;
constexpr int kVoltsScaleBase = 256;

constexpr uint16_t kOutputEnableBit = 1u << 15;
constexpr uint16_t kConstantCurrentBit = 1u << 0;

constexpr k_timeout_t kBusLockTimeout = K_MSEC(100);

// RAII wrapper around k_mutex with a bounded acquire timeout.
class ScopedLock {
 public:
  explicit ScopedLock(k_mutex* mutex)
      : mutex_(mutex), locked_(k_mutex_lock(mutex, kBusLockTimeout) == 0) {}
  ~ScopedLock() {
    if (locked_) {
      k_mutex_unlock(mutex_);
    }
  }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
  bool locked() const { return locked_; }

 private:
  k_mutex* mutex_;
  bool locked_;
};

}  // namespace

PsuController::PsuController(const device* i2c_dev, uint8_t address_index)
    : i2c_dev_(i2c_dev),
      device_address_(static_cast<uint8_t>(0x58 + address_index)) {
  k_mutex_init(&bus_mutex_);
}

bool PsuController::IsReady() const { return device_is_ready(i2c_dev_); }

bool PsuController::ReadFrame(uint8_t wire_reg, uint8_t* data, uint32_t count) {
  const uint16_t sum = static_cast<uint16_t>((device_address_ << 1) + wire_reg);
  const uint8_t checksum = static_cast<uint8_t>(-sum);
  uint8_t tx[2] = {wire_reg, checksum};

  ScopedLock lock(&bus_mutex_);
  if (!lock.locked()) {
    return false;
  }
  return i2c_write_read(i2c_dev_, device_address_, tx, sizeof(tx), data,
                        count) == 0;
}

bool PsuController::ReadRegister(uint8_t reg, int* value) {
  uint8_t data[3];
  if (!ReadFrame(static_cast<uint8_t>(reg << 1), data, sizeof(data))) {
    return false;
  }
  const uint16_t sum = static_cast<uint16_t>(data[0] + data[1]);
  const uint8_t checksum = static_cast<uint8_t>(-sum);
  if (checksum != data[2]) {
    return false;
  }
  *value = data[0] | (data[1] << 8);
  return true;
}

bool PsuController::WriteRegister(uint8_t reg, int value) {
  const uint8_t wire_reg = static_cast<uint8_t>(reg << 1);
  const uint8_t lsb = static_cast<uint8_t>(value & 0xFF);
  const uint8_t msb = static_cast<uint8_t>((value >> 8) & 0xFF);
  const uint16_t sum =
      static_cast<uint16_t>((device_address_ << 1) + wire_reg + lsb + msb);
  const uint8_t checksum = static_cast<uint8_t>(-sum);
  uint8_t payload[3] = {lsb, msb, checksum};

  ScopedLock lock(&bus_mutex_);
  if (!lock.locked()) {
    return false;
  }
  return i2c_burst_write(i2c_dev_, device_address_, wire_reg, payload,
                         sizeof(payload)) == 0;
}

bool PsuController::ReadMeasurement(Measurement* out) {
  bool ok = true;
  int val;
  if (ReadRegister(kRegVolts, &val)) {
    out->volts = static_cast<float>(val) / (OVP_SCALE_FACTOR * kVoltsScaleBase);
  } else {
    ok = false;
  }
  if (ReadRegister(kRegAmps, &val)) {
    out->amps = static_cast<float>(val) / kAmpsScale;
  } else {
    ok = false;
  }
  if (ReadRegister(kRegWatts, &val)) {
    out->watts = static_cast<float>(val);
  } else {
    ok = false;
  }
  return ok;
}

bool PsuController::SetFanRpm(int rpm) { return WriteRegister(kRegFanRpm, rpm); }

bool PsuController::GetOutputEnabled(bool* enabled) {
  int val;
  if (!ReadRegister(kRegStatus, &val)) {
    return false;
  }
  *enabled = (val & kOutputEnableBit) != 0;
  return true;
}

bool PsuController::ToggleOutput(bool* enabled_out) {
  int val;
  if (!ReadRegister(kRegStatus, &val)) {
    return false;
  }
  val ^= kOutputEnableBit;
  if (!WriteRegister(kRegStatus, val)) {
    return false;
  }
  if (!ReadRegister(kRegStatus, &val)) {
    return false;
  }
  *enabled_out = (val & kOutputEnableBit) != 0;
  return true;
}

bool PsuController::GetMode(OutputMode* mode) {
  int val;
  if (!ReadRegister(kRegStatus, &val)) {
    return false;
  }
  *mode = (val & kConstantCurrentBit) ? OutputMode::kConstantCurrent
                                      : OutputMode::kConstantVoltage;
  return true;
}

bool PsuController::ToggleMode(OutputMode* mode_out) {
  int val;
  if (!ReadRegister(kRegStatus, &val)) {
    return false;
  }
  val ^= kConstantCurrentBit;
  if (!WriteRegister(kRegStatus, val)) {
    return false;
  }
  if (!ReadRegister(kRegStatus, &val)) {
    return false;
  }
  *mode_out = (val & kConstantCurrentBit) ? OutputMode::kConstantCurrent
                                          : OutputMode::kConstantVoltage;
  return true;
}

bool PsuController::GetConstantCurrent(float* amps_out) {
  int val;
  if (!ReadRegister(kRegConstantCurrent, &val)) {
    return false;
  }
  *amps_out = static_cast<float>(val) / kCurrentScale;
  return true;
}

bool PsuController::SetConstantCurrent(float amps) {
  const int val = static_cast<uint16_t>(amps * kCurrentScale);
  return WriteRegister(kRegConstantCurrent, val);
}

}  // namespace psu
