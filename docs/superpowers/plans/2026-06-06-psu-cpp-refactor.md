# PSU Module C++ Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the procedural C `psu_ctrl` module into an object-oriented C++ module (`psu::PsuController` + a thin C adapter) using Google C++ Style, preserving behavior.

**Architecture:** A pure-logic `PsuController` class (I2C + DPS1200 protocol, no LVGL/zbus) in `src/psu/psu_controller.{hpp,cpp}`; a C-API adapter `src/psu/psu_service.{h,cpp}` (Meyers singleton + 500ms zbus poll + LVGL event callbacks + `SYS_INIT`); and the zbus channel definition isolated in a C file `src/psu/psu_channel.c` to avoid C++ macro issues.

**Tech Stack:** Zephyr RTOS, C++20 (full libcpp + exceptions + RTTI), LVGL, zbus, arm-none-eabi-g++, board `rpi_pico`, Ninja build in `build_lcd2`.

**No automated test framework exists (bare-metal embedded).** The red/green loop here is **build-based**: each task ends by compiling/linking and (for the switchover) checking the memory report. Hardware behavior is confirmed by the developer flashing `zephyr.uf2`.

**Standard build command (used in every verification step):**
```bash
cd /Users/litao/Developer/zpsu_mon && \
  export PATH="/Users/litao/anaconda3/bin:$PATH" && \
  export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr && \
  ninja -C build_lcd2
```
(Editing `CMakeLists.txt` makes Ninja auto-reconfigure on the next build. If new source files are NOT picked up, force a reconfigure with `cmake -B build_lcd2 -S . 2>/dev/null` then re-run ninja.)

---

## Task 1: Enable the C++ runtime and measure the memory baseline

Enabling full libcpp + exceptions + RTTI is the project's biggest risk (RAM is ~85% used). Turn it on first — with all sources still C — so its cost is measured in isolation before any refactor.

**Files:**
- Modify: `prj.conf`

- [ ] **Step 1: Record the current memory numbers**

Run the standard build command. Note the `RAM` and `FLASH` "Used Size" lines from the output (baseline before C++). Expected (from last known build): `FLASH ~466268 B (22.24%)`, `RAM ~230056 B (85.10%)`.

- [ ] **Step 2: Append the C++ Kconfig options to `prj.conf`**

Add these lines at the end of `prj.conf`:

```
# C++ support (PSU module refactor)
CONFIG_CPP=y
CONFIG_STD_CPP20=y
CONFIG_REQUIRES_FULL_LIBCPP=y
CONFIG_CPP_EXCEPTIONS=y
CONFIG_CPP_RTTI=y
```

- [ ] **Step 3: Rebuild and compare memory**

Run the standard build command.
Expected: build succeeds (still all C sources). Compare the new `RAM`/`FLASH` "Used Size" to Step 1. Record the delta — this is the pure C++-runtime cost.

> **Decision gate:** If `RAM` already overflows here (over 100%), STOP and report. The likely remedy is dropping `CONFIG_CPP_EXCEPTIONS=y` (and if still tight, `CONFIG_CPP_RTTI=y`). Do not proceed until the runtime fits.

- [ ] **Step 4: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add prj.conf
git commit -m "build: enable C++20 runtime for PSU module refactor

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Add the `PsuController` class (compiles alongside existing code)

The class is namespaced (`psu::`) with no symbol clash against the existing `psu_ctrl.c`, so it can be added to the build and compiled before anything depends on it.

**Files:**
- Create: `src/psu/psu_controller.hpp`
- Create: `src/psu/psu_controller.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create the class header**

Create `src/psu/psu_controller.hpp`:

```cpp
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
```

- [ ] **Step 2: Create the class implementation**

Create `src/psu/psu_controller.cpp`:

```cpp
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
```

- [ ] **Step 3: Add the class to the build**

In `CMakeLists.txt`, add `src/psu` to the existing `include_directories(...)` block so it reads:

```cmake
include_directories(
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/ui
  ${PROJECT_SOURCE_DIR}/src/psu
  )
```

In the `set(app_SRCS ...)` list, add this line (anywhere in the list, e.g. after the `psu_ctrl.c` line):

```cmake
  ${PROJECT_SOURCE_DIR}/src/psu/psu_controller.cpp
```

- [ ] **Step 4: Make a CMake-set `OVP_SCALE_FACTOR` reach C++ sources**

The existing block parses `OVP_SCALE_FACTOR` from `EXTRA_CFLAGS` but never turns it into a compile definition (the old C file got it via `EXTRA_CFLAGS`, which does not apply to C++). Right after the existing line `add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)`, add:

```cmake
if(OVP_SCALE_FACTOR)
  add_compile_definitions(OVP_SCALE_FACTOR=${OVP_SCALE_FACTOR})
endif()
```

(With nothing passed, `psu_controller.cpp` falls back to its `#define OVP_SCALE_FACTOR 1.0`, matching current default behavior.)

- [ ] **Step 5: Build to verify the class compiles**

Run the standard build command.
Expected: build succeeds. `psu_controller.cpp` compiles under C++20 (it is unreferenced for now; that is fine).

- [ ] **Step 6: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/psu/psu_controller.hpp src/psu/psu_controller.cpp CMakeLists.txt
git commit -m "feat(psu): add PsuController C++ class

Pure-logic DPS1200 controller (I2C + protocol + ops) in Google C++ style,
with named register/scale constants and an RAII bus lock. Not yet wired in.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Add the adapter + channel, switch all callers over, remove the old module

This is the atomic switchover: the old `psu_ctrl.c` and the new adapter both define `psuctrl_init`/the zbus channel/`SYS_INIT`, so they cannot coexist. Everything in this task lands in one commit that keeps the build green.

**Files:**
- Create: `src/psu/psu_channel.c`
- Create: `src/psu/psu_service.h`
- Create: `src/psu/psu_service.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/main.c:19-20`, `src/main.c:54-55`, `src/main.c:60`
- Modify: `src/ui/watchface_ui.c` (includes + line 30)
- Modify: `src/ui/screens/ui_Screen1.c:11-13,176,201,225-227`
- Delete: `src/psu_ctrl.c`, `src/psu_ctrl.h`

- [ ] **Step 1: Create the zbus channel C file**

The `ZBUS_CHAN_DEFINE` observer macros use compound literals that do not compile cleanly as C++, so keep the channel definition in C. Create `src/psu/psu_channel.c`:

```c
#include <zephyr/zbus/zbus.h>
#include <events/psuctrl_event.h>

/* Channel + observer kept here (C) so the zbus macros compile cleanly.
 * Name is unchanged: it is part of the contract observed by watchface_app.c. */
ZBUS_CHAN_DEFINE(psuctrl_data_chan,
                 struct psuctrl_data_event,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(watchface_psuctrl_event),
                 ZBUS_MSG_INIT());
```

- [ ] **Step 2: Create the C-API adapter header**

Create `src/psu/psu_service.h`:

```c
#ifndef PSU_PSU_SERVICE_H_
#define PSU_PSU_SERVICE_H_

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

int psu_service_init(void);   /* SYS_INIT entry */
void psu_ui_init(void);       /* initial label sync */

/* LVGL event callbacks (registered from the SquareLine-generated UI). */
void psu_power_toggle_event_cb(lv_event_t* e);
void psu_mode_toggle_event_cb(lv_event_t* e);
void psu_current_set_event_cb(lv_event_t* e);

int psu_set_fan_rpm(int rpm);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSU_PSU_SERVICE_H_ */
```

- [ ] **Step 3: Create the adapter implementation**

Create `src/psu/psu_service.cpp`:

```cpp
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
```

- [ ] **Step 4: Update `CMakeLists.txt` source list**

In the `set(app_SRCS ...)` list: REMOVE the line `${PROJECT_SOURCE_DIR}/src/psu_ctrl.c` and ADD these two lines:

```cmake
  ${PROJECT_SOURCE_DIR}/src/psu/psu_service.cpp
  ${PROJECT_SOURCE_DIR}/src/psu/psu_channel.c
```

(After this, `app_SRCS` contains `psu_controller.cpp` from Task 2, plus `psu_service.cpp` and `psu_channel.c`, and no longer `psu_ctrl.c`.)

- [ ] **Step 5: Update `src/main.c` include**

Replace the two duplicate include lines `src/main.c:19-20`:

```c
#include "psu_ctrl.h"
#include "psu_ctrl.h"
```

with a single line:

```c
#include "psu_service.h"
```

- [ ] **Step 6: Update the stale comments in `src/main.c`**

At `src/main.c:54-55`, update the commented thread reference (cosmetic, keeps names accurate):

```c
// K_THREAD_DEFINE(psuCtrlThreadId, CONFIG_BUTTON_PSUCTRL_STACK_SIZE,
//                 psu_service_init, NULL, NULL, NULL, CONFIG_BUTTON_PSUCTRL_THREAD_PRIO, 0, K_TICKS_FOREVER);
```

At `src/main.c:60`, update the comment:

```c
        // psu_service_init() is registered via SYS_INIT() in psu_service.cpp
```

- [ ] **Step 7: Update `src/ui/watchface_ui.c`**

Add an include alongside the existing includes at the top (after the `#include "ui.h"` line at `src/ui/watchface_ui.c:6`):

```c
#include "psu_service.h"
```

Change the call at `src/ui/watchface_ui.c:30` from:

```c
        PSUCtrl_UI_Init();
```

to:

```c
        psu_ui_init();
```

- [ ] **Step 8: Rename the callbacks in the generated `src/ui/screens/ui_Screen1.c`**

This file is SquareLine-generated; these edits keep the build linking now and MUST be mirrored in the SquareLine project (see the final task) or a future export will revert them.

Replace the three forward declarations (`src/ui/screens/ui_Screen1.c:11-13`):

```c
void PSUCtrl_ONOFF(lv_event_t * e);
void PSUCtrl_CVCC(lv_event_t * e);
void PSUCtrl_Set_CC(lv_event_t * e);
```

with:

```c
void psu_power_toggle_event_cb(lv_event_t * e);
void psu_mode_toggle_event_cb(lv_event_t * e);
void psu_current_set_event_cb(lv_event_t * e);
```

Change the callback registration at `src/ui/screens/ui_Screen1.c:176`:

```c
lv_obj_add_event_cb(ui_LabelOnOff, psu_power_toggle_event_cb, LV_EVENT_CLICKED, NULL);
```

Change `src/ui/screens/ui_Screen1.c:201`:

```c
lv_obj_add_event_cb(ui_LabelCVCC, psu_mode_toggle_event_cb, LV_EVENT_CLICKED, NULL);
```

Change `src/ui/screens/ui_Screen1.c:225` and the two commented lines `226-227`:

```c
lv_obj_add_event_cb(ui_lblPsuCc, psu_current_set_event_cb, LV_EVENT_KEY, NULL);
// lv_obj_add_event_cb(ui_lblPsuCc, psu_current_set_event_cb, LV_EVENT_LONG_PRESSED, NULL);
// lv_obj_add_event_cb(ui_lblPsuCc, psu_current_set_event_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
```

- [ ] **Step 9: Delete the old module**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/psu_ctrl.c src/psu_ctrl.h
```

- [ ] **Step 10: Build, link, and check memory**

Run the standard build command.
Expected: compiles AND links cleanly (no name-mangling/undefined-reference errors at the `extern "C"` boundary), and produces `zephyr.uf2`. Record the final `RAM`/`FLASH` "Used Size".

> **Decision gate (memory):** If `RAM` exceeds 100% (link fails with a region-overflow error), apply the fallback: in `prj.conf` change `CONFIG_CPP_EXCEPTIONS=y` to `CONFIG_CPP_EXCEPTIONS=n`, rebuild; if still overflowing, also set `CONFIG_CPP_RTTI=n`. Re-run the build and record the numbers. Note which options were disabled in the commit message.

> **Contingency (zbus in C++):** `psu_channel.c` is C precisely to avoid this, so the channel should compile. If any `ZBUS_*` macro error appears, confirm `psu_channel.c` (not the `.cpp`) holds the `ZBUS_CHAN_DEFINE`.

- [ ] **Step 11: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add -A
git commit -m "refactor(psu): replace psu_ctrl.c with PsuController + C adapter

Switch over to src/psu: psu_service (singleton, zbus poll, LVGL callbacks,
SYS_INIT) over the PsuController class, with the zbus channel in psu_channel.c.
Renames the LVGL event callbacks (psu_*_event_cb) and updates main.c,
watchface_ui.c, and the generated ui_Screen1.c call sites. Deletes psu_ctrl.{c,h}.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Delete zero-reference dead headers

**Files:**
- Delete: `src/ble_aoa.h`, `src/ble_transport.h`, `src/magnetometer.h`, `src/gpio_debug.h`

- [ ] **Step 1: Re-confirm the headers are unreferenced**

```bash
cd /Users/litao/Developer/zpsu_mon
grep -rn "ble_aoa.h\|ble_transport.h\|magnetometer.h\|gpio_debug.h" src \
  | grep -v -E "src/(ble_aoa|ble_transport|magnetometer|gpio_debug)\.h:"
```

Expected: no output (zero references). If any line prints, do NOT delete that header — remove it from the delete list and note it.

- [ ] **Step 2: Delete the confirmed-dead headers**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/ble_aoa.h src/ble_transport.h src/magnetometer.h src/gpio_debug.h
```

- [ ] **Step 3: Build to verify nothing broke**

Run the standard build command.
Expected: build succeeds, `zephyr.uf2` produced.

- [ ] **Step 4: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git commit -m "chore: remove zero-reference dead headers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Final verification and SquareLine sync hand-off

**Files:** none (verification + reporting)

- [ ] **Step 1: Clean rebuild and final memory report**

```bash
cd /Users/litao/Developer/zpsu_mon && \
  export PATH="/Users/litao/anaconda3/bin:$PATH" && \
  export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr && \
  ninja -C build_lcd2
```

Expected: clean build, `Wrote ... bytes to zephyr.uf2`. Record final `FLASH`/`RAM` "Used Size" and compare to the Task 1 baseline. Confirm `RAM` is under 100%.

- [ ] **Step 2: Confirm no stale old symbols remain in compiled sources**

```bash
cd /Users/litao/Developer/zpsu_mon
grep -rn "PSUCtrl_\|psuctrl_init" src --include=*.c --include=*.cpp --include=*.h --include=*.hpp
```

Expected: no matches (the `psuctrl_data_*` zbus names are allowed and differ — they contain `psuctrl_data`, not `psuctrl_init`/`PSUCtrl_`). If `PSUCtrl_` or `psuctrl_init` appears in a compiled source, fix it.

- [ ] **Step 3: Report the SquareLine sync checklist to the developer**

Output this for the developer to apply in the SquareLine project so a future UI export does not revert the callback names:

- `ui_LabelOnOff` CLICKED event -> `psu_power_toggle_event_cb`
- `ui_LabelCVCC` CLICKED event -> `psu_mode_toggle_event_cb`
- `ui_lblPsuCc` KEY event -> `psu_current_set_event_cb`

- [ ] **Step 4: Hand off for hardware verification**

Tell the developer to flash `build_lcd2/zephyr/zephyr.uf2` to the RP2040 (BOOTSEL mass-storage) and confirm: PSU volts/amps/watts/energy update on the watchface, the ON/OFF and CV/CC labels toggle on tap, and the CC value adjusts with UP/DOWN keys.

---

## Self-Review Notes (author check)

- **Spec coverage:** runtime config (§2/§9 → Task 1), `PsuController` class + Google style + named constants + RAII lock (§4 → Task 2), C adapter + renamed callbacks + Meyers singleton + zbus poll/energy + SYS_INIT (§3/§5 → Task 3), boundary kept (zbus names unchanged, generated/upstream untouched except required call-site renames → Task 3), dead-code removal (§7 → Tasks 3+4), behavior preservation (§8 → logic mirrored 1:1 in Task 2/3 code), verification + memory gate + SquareLine list (§5/§10/§11 → Tasks 1,3,5).
- **Type consistency:** method names/signatures in `psu_controller.hpp` (Task 2) match every call in `psu_service.cpp` (Task 3): `ReadMeasurement`, `ToggleOutput`, `ToggleMode`, `GetOutputEnabled`, `GetMode`, `GetConstantCurrent`, `SetConstantCurrent`, `SetFanRpm`. `OutputMode::kConstantCurrent/kConstantVoltage` used consistently.
- **Deviation from spec:** zbus channel moved to a dedicated C file `psu_channel.c` (spec §3 placed it in `psu_service.cpp`) to avoid C++ macro issues; C-API header is `psu_service.h` (not `.hpp`) since it is consumed by C files. Both improve robustness without changing the design intent.
