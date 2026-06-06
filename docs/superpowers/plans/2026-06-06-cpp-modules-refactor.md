# Convert Remaining C Modules to C++ Classes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert every remaining compiled C module (`main`, `watchface_app`, `watchface_ui`, `clock`, `notification_manager`, `display_control`, `led`) into an object-oriented C++ class in Google C++ style, behavior-preserving, following the proven `src/psu/` pattern.

**Architecture:** Each module becomes a C++ class (`namespace::ClassName`). Zephyr registration macros that don't compile cleanly as C++ (`ZBUS_LISTENER_DEFINE`, `BT_CONN_CB_DEFINE`) are isolated in a per-module C "glue" file forwarding to `extern "C"` hooks on the class (the `psu_channel.c` precedent). `main()` stays a free function delegating to an `App` class.

**Tech Stack:** Zephyr RTOS, C++20 (already enabled), LVGL, zbus, Bluetooth, arm-none-eabi-g++, board `rpi_pico`, Ninja build in `build_lcd2`.

**No automated test framework (bare-metal embedded).** Verification is build-based: each task ends by compiling + linking (`zephyr.uf2` produced). Hardware behavior is confirmed by the developer at the end.

**Standard build command (every verification step):**
```bash
cd /Users/litao/Developer/zpsu_mon && \
  export PATH="/Users/litao/anaconda3/bin:$PATH" && \
  export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr && \
  ninja -C build_lcd2
```
Timeout 600000 ms. Editing `CMakeLists.txt` triggers a Ninja reconfigure; if new files aren't picked up, run `cmake -B build_lcd2 -S . 2>/dev/null` then re-run ninja.

**Ordering rationale (top-down / caller-first):** A C file cannot call a C++ class method, but a C++ file can always call a C function via an `extern "C"` header. So we convert callers before callees: `main` → `watchface_app` → then the leaves it calls. When a leaf is converted, its (already-C++) caller is updated to the class API in the same task. Every commit links.

**Conversion-pattern reference (READ FIRST — applies to every task):**
- New class header `*.hpp`: `#ifndef <DIR>_<NAME>_HPP_` guard; one `namespace`; class with PascalCase methods, `trailing_` members, `kName` constants.
- New impl `*.cpp`: include its `.hpp` first; move each old function body **verbatim** into the matching method, with two mechanical substitutions only: (a) file-scope `static` variables become private members accessed as `member_`; (b) calls to other already-converted modules switch to their C++ class API (the per-task "Caller updates" list gives the exact replacements). Magic numbers already inline stay as-is unless the task says to name them. Do NOT change logic, add features, or "improve" behavior.
- Stateless modules use a class with `static` methods (a singleton is unnecessary).
- Stateful modules use a Meyers singleton: `static ClassName& Instance() { static ClassName x; return x; }`.
- `extern "C"` is needed ONLY for: functions referenced by Zephyr macros in a C glue file, and `main()`. Everything else is C++↔C++.
- After moving code out of a `.c`, `git rm` the old `.c`/`.h` and swap the entry in `CMakeLists.txt` `app_SRCS` (`.c` line removed, `.cpp` [+ glue `.c`] line added). Add each new source dir to `include_directories(...)`.

---

## Task 1: `main.c` → `app::App` (+ thin `main.cpp`)

**Files:**
- Create: `src/app/app.hpp`, `src/app/app.cpp`
- Create: `src/main.cpp` (thin); Delete: `src/main.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/app/app.hpp`**

```cpp
#ifndef APP_APP_HPP_
#define APP_APP_HPP_

namespace app {

// Top-level application: owns the boot/init sequence.
class App {
 public:
  static App& Instance();

  void Start();    // submit init work to the system workqueue (called from main)
  void RunInit();  // runs on the system workqueue

 private:
  App() = default;
  int InitRgbPwmLed();
};

}  // namespace app

#endif  // APP_APP_HPP_
```

- [ ] **Step 2: Create `src/app/app.cpp`**

This is `main.c`'s `run_init_work` + `pwm_rgb_led_init` moved into `App`, with `run_init_work`'s stray `return 0;` (illegal in a `void`/C++ context) corrected to `return;`.

```cpp
#include "app.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display_control.h"
#include "watchface_app.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_WRN);

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

App& App::Instance() {
  static App instance;
  return instance;
}

void App::Start() {
  // The init code needs stack; reuse the system workqueue for init instead of
  // growing CONFIG_MAIN_STACK_SIZE.
  k_work_submit(&init_work);
}

int App::InitRgbPwmLed() {
  uint32_t pulse_red = 0, pulse_green = 0, pulse_blue = 0;
  int ret;

  printk("PWM-based RGB LED control\n");

  if (!device_is_ready(red_pwm_led.dev) ||
      !device_is_ready(green_pwm_led.dev) ||
      !device_is_ready(blue_pwm_led.dev)) {
    printk("Error: one or more PWM devices not ready\n");
    return 0;
  }

  ret = pwm_set_pulse_dt(&red_pwm_led, pulse_red);
  if (ret != 0) {
    printk("Error %d: red write failed\n", ret);
    return 0;
  }
  ret = pwm_set_pulse_dt(&green_pwm_led, pulse_green);
  if (ret != 0) {
    printk("Error %d: green write failed\n", ret);
    return 0;
  }
  ret = pwm_set_pulse_dt(&blue_pwm_led, pulse_blue);
  if (ret != 0) {
    printk("Error %d: blue write failed\n", ret);
    return 0;
  }
  return 0;
}

void App::RunInit() {
  InitRgbPwmLed();
  // psu_service_init() is registered via SYS_INIT() in psu_service.cpp
  display_control_init();
  display_control_power_on(true);

  const struct device* lvgl_btn_dev =
      DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_button_input));
  if (!device_is_ready(lvgl_btn_dev)) {
    LOG_ERR("Device not ready, aborting...");
    return;
  }

  watchface_app_start(NULL);
}

}  // namespace app
```

- [ ] **Step 3: Create `src/main.cpp`**

```cpp
#include "app.hpp"

int main(void) {
  app::App::Instance().Start();
  return 0;
}
```

- [ ] **Step 4: Delete the old `main.c` and update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/main.c
```
In `CMakeLists.txt`: add `${PROJECT_SOURCE_DIR}/src/app` to `include_directories(...)`. In `set(app_SRCS ...)`, remove `${PROJECT_SOURCE_DIR}/src/main.c` and add:
```cmake
  ${PROJECT_SOURCE_DIR}/src/main.cpp
  ${PROJECT_SOURCE_DIR}/src/app/app.cpp
```

- [ ] **Step 5: Build**

Run the standard build command. Expected: compiles + links, `Wrote ... zephyr.uf2`. (`app.cpp` calls the still-C `display_control_*` and `watchface_app_start` via their `extern "C"` headers — both already have guards.)

- [ ] **Step 6: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/app/app.hpp src/app/app.cpp src/main.cpp CMakeLists.txt
git commit -m "refactor(app): convert main.c to app::App + thin main.cpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `watchface_app.c` → `app::WatchfaceApp` (+ C glue for zbus/BT macros)

**Files:**
- Create: `src/app/watchface_app.hpp`, `src/app/watchface_app.cpp`, `src/app/watchface_app_hooks.h`, `src/app/watchface_app_glue.c`
- Delete: `src/watchface_app.c`, `src/watchface_app.h`
- Modify: `CMakeLists.txt`; `src/app/app.cpp` (call the class); add `extern "C"` guards to `src/clock.h`, `src/notification_manager.h`, `src/ui/watchface_ui.h`

**Reference:** original `src/watchface_app.c` (read it). The conversion keeps ALL logic identical, including the `#if 0` battery/sensor stubs and the commented-out work scheduling. Only the structure changes:
- file-statics (`running`, `last_data_update`, the `delayed_work_item_t` items, `general_work_item`, `canel_work_sync`, `levels[]`) → kept as file-statics in the anonymous namespace of `watchface_app.cpp` (they are work-queue plumbing, not class API), EXCEPT `running` which becomes the class member `running_`.
- the 4 zbus listener callbacks + 2 BT callbacks move to `extern "C"` hooks (declared in `watchface_app_hooks.h`, implemented in `watchface_app.cpp`) and are wired by `ZBUS_LISTENER_DEFINE`/`BT_CONN_CB_DEFINE` in `watchface_app_glue.c`.
- `watchface_app_init` (SYS_INIT) calls `WatchfaceApp::Instance().Init()`.

- [ ] **Step 1: Create `src/app/watchface_app_hooks.h`** (shared C/C++ declarations of the glue forwarders)

```c
#ifndef APP_WATCHFACE_APP_HOOKS_H_
#define APP_WATCHFACE_APP_HOOKS_H_

#include <stdint.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/bluetooth/conn.h>

#ifdef __cplusplus
extern "C" {
#endif

void watchface_app_on_ble_comm(const struct zbus_channel* chan);
void watchface_app_on_accel(const struct zbus_channel* chan);
void watchface_app_on_chg(const struct zbus_channel* chan);
void watchface_app_on_psu(const struct zbus_channel* chan);
void watchface_app_on_connected(struct bt_conn* conn, uint8_t err);
void watchface_app_on_disconnected(struct bt_conn* conn, uint8_t reason);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APP_WATCHFACE_APP_HOOKS_H_ */
```

- [ ] **Step 2: Create `src/app/watchface_app_glue.c`** (C — the Zephyr macros that don't compile cleanly as C++)

```c
#include <zephyr/zbus/zbus.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include "watchface_app_hooks.h"

ZBUS_CHAN_DECLARE(ble_comm_data_chan);
ZBUS_LISTENER_DEFINE(watchface_ble_comm_lis, watchface_app_on_ble_comm);

ZBUS_CHAN_DECLARE(accel_data_chan);
ZBUS_LISTENER_DEFINE(watchface_accel_lis, watchface_app_on_accel);

ZBUS_CHAN_DECLARE(chg_state_data_chan);
ZBUS_LISTENER_DEFINE(watchface_chg_event, watchface_app_on_chg);

ZBUS_CHAN_DECLARE(psuctrl_data_chan);
ZBUS_LISTENER_DEFINE(watchface_psuctrl_event, watchface_app_on_psu);

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = watchface_app_on_connected,
    .disconnected = watchface_app_on_disconnected,
};
```

- [ ] **Step 3: Create `src/app/watchface_app.hpp`**

```cpp
#ifndef APP_WATCHFACE_APP_HPP_
#define APP_WATCHFACE_APP_HPP_

#include <lvgl.h>

namespace app {

// Watchface coordinator: zbus listeners, BT connection callbacks, and the
// periodic work that drives the watchface UI.
class WatchfaceApp {
 public:
  static WatchfaceApp& Instance();

  void Init();                  // SYS_INIT: init work items, running_ = false
  void Start(lv_group_t* group);
  void Stop();

  bool running() const { return running_; }

  // Forwarded from the C glue (zbus listeners / BT callbacks).
  void HandleBleComm(const struct zbus_channel* chan);
  void HandleAccel(const struct zbus_channel* chan);
  void HandleChg(const struct zbus_channel* chan);
  void HandlePsu(const struct zbus_channel* chan);
  void HandleConnected(struct bt_conn* conn, uint8_t err);
  void HandleDisconnected(struct bt_conn* conn, uint8_t reason);

 private:
  WatchfaceApp() = default;
  bool running_ = false;
};

}  // namespace app

#endif  // APP_WATCHFACE_APP_HPP_
```

- [ ] **Step 4: Create `src/app/watchface_app.cpp`** (transcribe original bodies; structure shown verbatim)

Includes + module-local state + work plumbing (transcribe `general_work`, `read_battery`, `check_notifications`, `update_ui_from_event`, the work items, and `levels[]` **verbatim** from `src/watchface_app.c` into the anonymous namespace; they are unchanged). Then the class + `extern "C"` hooks + `SYS_INIT`. The exact scaffolding:

```cpp
#include "watchface_app.hpp"

#include "watchface_app_hooks.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <clock.h>
#include <battery.h>
#include <heart_rate_sensor.h>
#include <accelerometer.h>
#include <vibration_motor.h>
#include <ram_retention_storage.h>
#include <events/ble_data_event.h>
#include <events/accel_event.h>
#include <notification_manager.h>
#include <zephyr/zbus/zbus.h>
#include <zsw_charger.h>
#include <events/chg_event.h>
#include <events/psuctrl_event.h>
#include <watchface_ui.h>
#include <display_control.h>   // for lvgl_update(); C++ forbids implicit declaration

LOG_MODULE_REGISTER(watcface_app, LOG_LEVEL_WRN);

#define WORK_STACK_SIZE 3000
#define WORK_PRIORITY   5
#define RENDER_INTERVAL_LVGL    K_MSEC(100)
#define ACCEL_INTERVAL          K_MSEC(100)
#define BATTERY_INTERVAL        K_MINUTES(1)
#define SEND_STATUS_INTERVAL    K_SECONDS(30)
#define DATE_UPDATE_INTERVAL    K_MINUTES(1)

namespace {

typedef enum work_type {
    UPDATE_CLOCK, OPEN_WATCHFACE, BATTERY, SEND_STATUS_UPDATE, PSU_STATUS_UPDATE, UPDATE_DATE
} work_type_t;

typedef struct delayed_work_item {
    struct k_work_delayable work;
    work_type_t             type;
} delayed_work_item_t;

void general_work(struct k_work* item);
void update_ui_from_event(struct k_work* item);
void check_notifications(void);
int read_battery(int* batt_mV, int* percent);

delayed_work_item_t battery_work = { .type = BATTERY };
delayed_work_item_t clock_work   = { .type = UPDATE_CLOCK };
delayed_work_item_t status_work  = { .type = SEND_STATUS_UPDATE };
delayed_work_item_t date_work    = { .type = UPDATE_DATE };
delayed_work_item_t psuctrl_work = { .type = PSU_STATUS_UPDATE };
delayed_work_item_t general_work_item;
struct k_work_sync canel_work_sync;
K_WORK_DEFINE(update_ui_work, update_ui_from_event);
ble_comm_cb_data_t last_data_update;

// ==== TRANSCRIBE VERBATIM FROM src/watchface_app.c ====
// - `levels[]` (battery discharge curve)
// - body of `general_work(struct k_work *item)`
// - body of `read_battery(int*, int*)`
// - body of `check_notifications(void)`
// - body of `update_ui_from_event(struct k_work *item)`
// In all of them, replace the bare global `running` with `app::WatchfaceApp::Instance().running()`.
// Leave the `#if 0` blocks and commented-out scheduling exactly as-is.
// ======================================================

}  // namespace

namespace app {

WatchfaceApp& WatchfaceApp::Instance() {
  static WatchfaceApp instance;
  return instance;
}

void WatchfaceApp::Init() {
  k_work_init_delayable(&general_work_item.work, general_work);
  k_work_init_delayable(&battery_work.work, general_work);
  k_work_init_delayable(&clock_work.work, general_work);
  k_work_init_delayable(&status_work.work, general_work);
  k_work_init_delayable(&date_work.work, general_work);
  running_ = false;
}

void WatchfaceApp::Start(lv_group_t* group) {
  ARG_UNUSED(group);
  general_work_item.type = OPEN_WATCHFACE;
  __ASSERT(0 <= k_work_schedule(&general_work_item.work, K_NO_WAIT), "FAIL schedule");
}

void WatchfaceApp::Stop() {
  running_ = false;
  k_work_cancel_delayable_sync(&battery_work.work, &canel_work_sync);
  k_work_cancel_delayable_sync(&clock_work.work, &canel_work_sync);
  k_work_cancel_delayable_sync(&date_work.work, &canel_work_sync);
  watchface_remove();
}

// HandleBleComm/HandleAccel/HandleChg/HandlePsu/HandleConnected/HandleDisconnected:
// TRANSCRIBE the bodies of the original zbus_*_callback and connected/disconnected
// functions VERBATIM here, replacing the bare `running` with `running_` and
// `running = true/false` (in OPEN_WATCHFACE/connected paths) accordingly. The OPEN_WATCHFACE
// `running = true` assignment stays inside general_work via Instance(); see note below.

void WatchfaceApp::HandleBleComm(const struct zbus_channel* chan) {
  if (running_) {
    struct ble_data_event* event = (struct ble_data_event*)zbus_chan_msg(chan);
    memcpy(&last_data_update, &event->data, sizeof(ble_comm_cb_data_t));
    k_work_submit(&update_ui_work);
  }
}

void WatchfaceApp::HandleAccel(const struct zbus_channel* chan) {
  if (running_) {
    struct accel_event* event = (struct accel_event*)zbus_chan_msg(chan);
    if (event->data.type == ACCELEROMETER_EVT_TYPE_STEP) {
      watchface_set_step(event->data.data.step.count);
    }
  }
}

void WatchfaceApp::HandleChg(const struct zbus_channel* chan) {
  if (running_) {
    struct chg_state_event* event = (struct chg_state_event*)zbus_chan_msg(chan);
    LOG_WRN("CHG: %d", event->is_charging);
    __ASSERT(0 <= k_work_reschedule(&status_work.work, K_MSEC(10)), "Failed schedule status work");
  }
}

void WatchfaceApp::HandlePsu(const struct zbus_channel* chan) {
  if (running_) {
    struct psuctrl_data_event* event = (struct psuctrl_data_event*)zbus_chan_msg(chan);
    watchface_set_ep(event);
    lvgl_update();
  }
}

void WatchfaceApp::HandleConnected(struct bt_conn* conn, uint8_t err) {
  if (!running_) return;
  if (err) {
    LOG_ERR("Connection failed (err %u)", err);
    return;
  }
  __ASSERT(0 <= k_work_schedule(&status_work.work, K_MSEC(1000)), "FAIL status");
  watchface_set_ble_connected(true);
}

void WatchfaceApp::HandleDisconnected(struct bt_conn* conn, uint8_t reason) {
  if (!running_) return;
  watchface_set_ble_connected(false);
}

}  // namespace app

extern "C" {
void watchface_app_on_ble_comm(const struct zbus_channel* chan) { app::WatchfaceApp::Instance().HandleBleComm(chan); }
void watchface_app_on_accel(const struct zbus_channel* chan)    { app::WatchfaceApp::Instance().HandleAccel(chan); }
void watchface_app_on_chg(const struct zbus_channel* chan)      { app::WatchfaceApp::Instance().HandleChg(chan); }
void watchface_app_on_psu(const struct zbus_channel* chan)      { app::WatchfaceApp::Instance().HandlePsu(chan); }
void watchface_app_on_connected(struct bt_conn* conn, uint8_t err)    { app::WatchfaceApp::Instance().HandleConnected(conn, err); }
void watchface_app_on_disconnected(struct bt_conn* conn, uint8_t reason) { app::WatchfaceApp::Instance().HandleDisconnected(conn, reason); }
}  // extern "C"

static int watchface_app_init(const struct device* arg) {
  ARG_UNUSED(arg);
  app::WatchfaceApp::Instance().Init();
  return 0;
}
SYS_INIT(watchface_app_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

Note on `general_work` (transcribed in the anonymous namespace): in the `OPEN_WATCHFACE` case the original sets `running = true;` — replace with `app::WatchfaceApp::Instance().Init` is NOT correct; instead expose it by writing `running` through the instance. Simplest faithful transcription: in `general_work`, replace `running = true;` with a call to a tiny helper — add a public method `void set_running(bool v) { running_ = v; }` to the class and call `app::WatchfaceApp::Instance().set_running(true);`. Add that method to `watchface_app.hpp` (public). All read sites use `running()`.

- [ ] **Step 5: Add `set_running` to `watchface_app.hpp`**

In the `public:` section of `WatchfaceApp`, add:
```cpp
  void set_running(bool value) { running_ = value; }
```

- [ ] **Step 6: Add `extern "C"` guards to the leaf headers this file calls**

`watchface_app.cpp` (C++) calls the still-C `clock_get_time`, `notification_manager_get_num`, and `watchface_*`. Wrap their declarations so they link. In each of `src/clock.h`, `src/notification_manager.h`, `src/ui/watchface_ui.h`: immediately after the existing `#include` lines, add `#ifdef __cplusplus`/`extern "C" {`/`#endif`, and before the final `#endif` add the closing `#ifdef __cplusplus`/`}`/`#endif`. (`src/display_control.h` already has guards.) Example for `src/clock.h`:
```c
#ifndef __CLOCK_H
#define __CLOCK_H
#include <inttypes.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void clock_init(uint64_t start_time_seconds);
struct tm *clock_get_time(void);
time_t clock_get_time_unix(void);

#ifdef __cplusplus
}
#endif

#endif
```
Apply the analogous wrap to `notification_manager.h` (around its function declarations) and `watchface_ui.h` (around all its `watchface_*` declarations).

- [ ] **Step 7: Update `src/app/app.cpp` to call the class**

`app.cpp` is now C++; switch from the C API to the class. Replace `#include "watchface_app.h"` with `#include "watchface_app.hpp"`, and replace `watchface_app_start(NULL);` with:
```cpp
  app::WatchfaceApp::Instance().Start(nullptr);
```

- [ ] **Step 8: Delete old files and update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/watchface_app.c src/watchface_app.h
```
In `CMakeLists.txt` `app_SRCS`: remove `${PROJECT_SOURCE_DIR}/src/watchface_app.c`; add:
```cmake
  ${PROJECT_SOURCE_DIR}/src/app/watchface_app.cpp
  ${PROJECT_SOURCE_DIR}/src/app/watchface_app_glue.c
```

- [ ] **Step 9: Build**

Run the standard build command. Expected: compiles + links (the zbus/BT macros live in the C glue file; the hooks resolve across the `extern "C"` boundary), `zephyr.uf2` produced.

If a `ZBUS_LISTENER_DEFINE` or `BT_CONN_CB_DEFINE` error appears, confirm it is in `watchface_app_glue.c` (C), not the `.cpp`. If an undefined-reference to a `watchface_app_on_*` hook appears, confirm the `extern "C"` block in `watchface_app.cpp` defines all six with exact signatures from `watchface_app_hooks.h`.

- [ ] **Step 10: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add -A src/app src/clock.h src/notification_manager.h src/ui/watchface_ui.h CMakeLists.txt
git commit -m "refactor(app): convert watchface_app.c to app::WatchfaceApp + C glue

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```
(Note: `git add -A src/app` stages the new files; the explicit paths avoid sweeping unrelated untracked artifacts. Do NOT use a bare `git add -A`.)

---

## Task 3: `watchface_ui.c` → `ui::Watchface`

**Files:**
- Create: `src/ui/watchface.hpp`, `src/ui/watchface.cpp`
- Delete: `src/ui/watchface_ui.c`, `src/ui/watchface_ui.h`
- Modify: `CMakeLists.txt`; `src/app/watchface_app.cpp` (call the class)

`watchface_ui.c` is almost entirely empty stubs; only `watchface_set_ep` and the `format_val` helper have real logic. The class is **stateless** → `static` methods. The unused, body-less `get_icon_from_weather_code` and `page_event_cb` stubs are dropped (never called). The GCC statement-expression `max` macro is replaced with `std::max` (identical behavior).

- [ ] **Step 1: Create `src/ui/watchface.hpp`**

```cpp
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
```

- [ ] **Step 2: Create `src/ui/watchface.cpp`**

```cpp
#include "watchface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <lvgl.h>
#include <zephyr/logging/log.h>

#include "ui.h"
#include "psu_service.h"

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
```

- [ ] **Step 3: Update `src/app/watchface_app.cpp` to call `ui::Watchface`**

Replace `#include <watchface_ui.h>` with `#include "watchface.hpp"`. Then replace every `watchface_*` call with the class method (these appear in the transcribed `general_work`, in `Stop()`, in the `Handle*` methods, and in `check_notifications`/`update_ui_from_event`):

| Old call | New call |
|---|---|
| `watchface_show()` | `ui::Watchface::Show()` |
| `watchface_remove()` | `ui::Watchface::Remove()` |
| `watchface_set_time(h, m, s)` | `ui::Watchface::SetTime(h, m, s)` |
| `watchface_set_date(wday, mday)` | `ui::Watchface::SetDate(wday, mday)` |
| `watchface_set_battery_percent(p, v)` | `ui::Watchface::SetBatteryPercent(p, v)` |
| `watchface_set_hrm(x)` | `ui::Watchface::SetHrm(x)` |
| `watchface_set_num_notifcations(x)` | `ui::Watchface::SetNumNotifications(x)` |
| `watchface_set_step(x)` | `ui::Watchface::SetStep(x)` |
| `watchface_set_ep(event)` | `ui::Watchface::SetEnergyPanel(event)` |
| `watchface_set_ble_connected(b)` | `ui::Watchface::SetBleConnected(b)` |
| `watchface_set_weather(t, code)` | `ui::Watchface::SetWeather(t, code)` |

- [ ] **Step 4: Delete old files, update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/ui/watchface_ui.c src/ui/watchface_ui.h
```
In `app_SRCS`: remove `${PROJECT_SOURCE_DIR}/src/ui/watchface_ui.c`; add `${PROJECT_SOURCE_DIR}/src/ui/watchface.cpp`. (`src/ui` is already in `include_directories`.)

- [ ] **Step 5: Build**

Run the standard build command. Expected: compiles + links, `zephyr.uf2` produced.

- [ ] **Step 6: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/ui/watchface.hpp src/ui/watchface.cpp src/app/watchface_app.cpp CMakeLists.txt
git commit -m "refactor(ui): convert watchface_ui.c to ui::Watchface

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: `clock.c` → `timekeeping::Clock`

**Files:**
- Create: `src/clock/clock.hpp`, `src/clock/clock.cpp`
- Delete: `src/clock.c`, `src/clock.h`
- Modify: `CMakeLists.txt`; `src/app/watchface_app.cpp` (call the class)

Stateless POSIX-time wrapper → `static` methods. (Namespace `timekeeping` avoids clashing with `<time.h>`'s `clock`.)

- [ ] **Step 1: Create `src/clock/clock.hpp`**

```cpp
#ifndef CLOCK_CLOCK_HPP_
#define CLOCK_CLOCK_HPP_

#include <stdint.h>
#include <time.h>

namespace timekeeping {

class Clock {
 public:
  static void Init(uint64_t start_time_seconds);
  static struct tm* GetTime();
  static time_t GetTimeUnix();
};

}  // namespace timekeeping

#endif  // CLOCK_CLOCK_HPP_
```

- [ ] **Step 2: Create `src/clock/clock.cpp`** (bodies transcribed verbatim from `clock.c`, `nullptr` for `NULL`)

```cpp
#include "clock.hpp"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace timekeeping {

void Clock::Init(uint64_t start_time_seconds) {
  setenv("TZ", "CET-1CEST", 1);
  tzset();

  struct timespec tspec;
  tspec.tv_sec = start_time_seconds;
  tspec.tv_nsec = 0;
  clock_settime(CLOCK_REALTIME, &tspec);
}

struct tm* Clock::GetTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return localtime(&tv.tv_sec);
}

time_t Clock::GetTimeUnix() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec;
}

}  // namespace timekeeping
```

- [ ] **Step 3: Update `src/app/watchface_app.cpp`**

Replace `#include <clock.h>` with `#include "clock.hpp"`. Replace both `clock_get_time()` calls (in the `UPDATE_CLOCK` and `UPDATE_DATE` cases of the transcribed `general_work`) with `timekeeping::Clock::GetTime()`. (`clock_get_time_unix`/`clock_init` are not called anywhere; the class still provides them.)

- [ ] **Step 4: Delete old files, update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/clock.c src/clock.h
```
In `CMakeLists.txt`: add `${PROJECT_SOURCE_DIR}/src/clock` to `include_directories(...)`. In `app_SRCS`: remove `${PROJECT_SOURCE_DIR}/src/clock.c`; add `${PROJECT_SOURCE_DIR}/src/clock/clock.cpp`.

- [ ] **Step 5: Build**

Run the standard build command. Expected: compiles + links.

- [ ] **Step 6: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/clock/clock.hpp src/clock/clock.cpp src/app/watchface_app.cpp CMakeLists.txt
git commit -m "refactor(clock): convert clock.c to timekeeping::Clock

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: `notification_manager.c` → `notify::NotificationManager`

**Files:**
- Create: `src/notification/notification_manager.hpp`, `src/notification/notification_manager.cpp`
- Delete: `src/notification_manager.c`, `src/notification_manager.h`
- Modify: `CMakeLists.txt`; `src/app/watchface_app.cpp` (call the class)

Stateful (notification store) → Meyers singleton. The public data types (struct/enum/constants) stay in the header. The header-declared `notification_manager_get` has **no definition** in the original `.c` (dangling declaration, never called) — it is omitted from the class.

- [ ] **Step 1: Create `src/notification/notification_manager.hpp`**

```cpp
#ifndef NOTIFICATION_NOTIFICATION_MANAGER_HPP_
#define NOTIFICATION_NOTIFICATION_MANAGER_HPP_

#include <inttypes.h>

#include <ble_comm.h>

#define NOTIFICATION_MGR_MAX_FIELD_LEN  50
#define NOTIFICATION_MANAGER_MAX_STORED 5

typedef enum notification_src {
    NOTIFICATION_SRC_MESSENGER,
    NOTIFICATION_SRC_GMAIL,
    NOTIFICATION_SRC_NONE
} notification_src_t;

typedef struct not_mngr_notification {
    uint32_t id;
    char sender[NOTIFICATION_MGR_MAX_FIELD_LEN];
    char title[NOTIFICATION_MGR_MAX_FIELD_LEN];
    char body[NOTIFICATION_MGR_MAX_FIELD_LEN];
    notification_src_t src;
} not_mngr_notification_t;

namespace notify {

class NotificationManager {
 public:
  static NotificationManager& Instance();

  void Init();
  not_mngr_notification_t* Add(ble_comm_notify_t* notification);
  int32_t Remove(uint32_t id);
  int32_t GetAll(not_mngr_notification_t* notifications, int* num_notifications);
  int32_t GetNum();
  not_mngr_notification_t* GetNewest();

 private:
  NotificationManager() = default;
  uint32_t FindIdx(uint32_t id);
  uint32_t FindFreeIdx();
  uint32_t FindOldestIdx();
  uint32_t FindNewestIdx();

  not_mngr_notification_t notifications_[NOTIFICATION_MANAGER_MAX_STORED];
  uint8_t num_notifications_ = 0;
  not_mngr_notification_t* active_notification_ = nullptr;
};

}  // namespace notify

#endif  // NOTIFICATION_NOTIFICATION_MANAGER_HPP_
```

- [ ] **Step 2: Create `src/notification/notification_manager.cpp`**

Transcribe the bodies of `notification_manager_init`, `_add`, `_remove`, `_get_all`, `_get_num`, `_get_newest`, and the four `find_*` helpers **verbatim** from `src/notification_manager.c` into the corresponding methods, with these mechanical substitutions: the file-statics become members (`notifications` → `notifications_`, `num_notifications` → `num_notifications_`, `active_notification` → `active_notification_`); the `find_*` calls become the private `Find*` methods. Scaffolding + the two short methods shown to fix names exactly:

```cpp
#include "notification_manager.hpp"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>  // MIN, __ASSERT_NO_MSG

#define NOTIFICATION_INVALID_ID    0xFFFFFFFF
#define NOTIFICATION_INVALID_INDEX 0xFFFFFFFF

namespace notify {

NotificationManager& NotificationManager::Instance() {
  static NotificationManager instance;
  return instance;
}

void NotificationManager::Init() {
  memset(notifications_, 0, sizeof(notifications_));
  for (int i = 0; i < NOTIFICATION_MANAGER_MAX_STORED; i++) {
    notifications_[i].id = NOTIFICATION_INVALID_ID;
  }
  num_notifications_ = 0;
  active_notification_ = nullptr;
}

int32_t NotificationManager::GetNum() { return num_notifications_; }

// Add(), Remove(), GetAll(), GetNewest(), FindIdx(), FindFreeIdx(),
// FindOldestIdx(), FindNewestIdx():
//   Transcribe verbatim from src/notification_manager.c with the member-name
//   substitutions above. In Add(), the helper calls become FindFreeIdx()/
//   FindOldestIdx(); in Remove(), FindIdx(); in GetNewest(), FindNewestIdx().
//   Keep the `MIN(...)` calls and the strncmp/memcpy logic exactly as-is.

}  // namespace notify
```

- [ ] **Step 3: Update `src/app/watchface_app.cpp`**

Replace `#include <notification_manager.h>` with `#include "notification_manager.hpp"`. In the transcribed `check_notifications()`, replace `notification_manager_get_num()` with `notify::NotificationManager::Instance().GetNum()`.

- [ ] **Step 4: Delete old files, update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/notification_manager.c src/notification_manager.h
```
In `CMakeLists.txt`: add `${PROJECT_SOURCE_DIR}/src/notification` to `include_directories(...)`. In `app_SRCS`: remove `${PROJECT_SOURCE_DIR}/src/notification_manager.c`; add `${PROJECT_SOURCE_DIR}/src/notification/notification_manager.cpp`.

- [ ] **Step 5: Build**

Run the standard build command. Expected: compiles + links.

- [ ] **Step 6: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/notification/notification_manager.hpp src/notification/notification_manager.cpp src/app/watchface_app.cpp CMakeLists.txt
git commit -m "refactor(notify): convert notification_manager.c to notify::NotificationManager

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: `display_control.c` → `display::DisplayControl`

**Files:**
- Create: `src/display/display_control.hpp`, `src/display/display_control.cpp`
- Delete: `src/display_control.c`, `src/display_control.h`
- Modify: `CMakeLists.txt`; `src/app/app.cpp` and `src/app/watchface_app.cpp` (call the class)

Stateful (`is_on`, `last_brightness`) → Meyers singleton.

- [ ] **Step 1: Create `src/display/display_control.hpp`**

```cpp
#ifndef DISPLAY_DISPLAY_CONTROL_HPP_
#define DISPLAY_DISPLAY_CONTROL_HPP_

#include <cstdint>

namespace display {

class DisplayControl {
 public:
  static DisplayControl& Instance();

  void Init();
  void PowerOn(bool on);
  void SetBrightness(uint8_t percent);
  void LvglUpdate();

 private:
  DisplayControl() = default;
  bool is_on_ = false;
  uint8_t last_brightness_ = 80;
};

}  // namespace display

#endif  // DISPLAY_DISPLAY_CONTROL_HPP_
```

- [ ] **Step 2: Create `src/display/display_control.cpp`**

File-statics (device specs + work) shown verbatim; method bodies transcribed verbatim from `display_control.c` with `is_on`→`is_on_`, `last_brightness`→`last_brightness_`, `display_control_set_brightness(x)`→`SetBrightness(x)`.

```cpp
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

}  // namespace

namespace display {

DisplayControl& DisplayControl::Instance() {
  static DisplayControl instance;
  return instance;
}

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
    SetBrightness(last_brightness_);
    k_work_schedule(&lvgl_work, K_MSEC(1));
  } else {
    if (device_is_ready(reg_dev)) {
      regulator_disable(reg_dev);
      pm_device_action_run(display_dev, PM_DEVICE_ACTION_TURN_OFF);
    } else {
      pm_device_action_run(display_dev, PM_DEVICE_ACTION_SUSPEND);
    }
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

}  // namespace display
```
(`zsw_cpu_freq.h` from the original is dropped — it is not referenced. If any retained Zephyr include fails under C++ and is unused, drop it and note which.)

- [ ] **Step 3: Update callers**

In `src/app/app.cpp`: replace `#include "display_control.h"` with `#include "display_control.hpp"`; `display_control_init();` → `display::DisplayControl::Instance().Init();`; `display_control_power_on(true);` → `display::DisplayControl::Instance().PowerOn(true);`.

In `src/app/watchface_app.cpp`: replace `#include <display_control.h>` with `#include "display_control.hpp"`; in `HandlePsu`, `lvgl_update();` → `display::DisplayControl::Instance().LvglUpdate();`.

- [ ] **Step 4: Delete old files, update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/display_control.c src/display_control.h
```
In `CMakeLists.txt`: add `${PROJECT_SOURCE_DIR}/src/display` to `include_directories(...)`. In `app_SRCS`: remove `${PROJECT_SOURCE_DIR}/src/display_control.c`; add `${PROJECT_SOURCE_DIR}/src/display/display_control.cpp`.

- [ ] **Step 5: Build**

Run the standard build command. Expected: compiles + links.

- [ ] **Step 6: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/display/display_control.hpp src/display/display_control.cpp src/app/app.cpp src/app/watchface_app.cpp CMakeLists.txt
git commit -m "refactor(display): convert display_control.c to display::DisplayControl

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: `led.c` → `led::Led`

**Files:**
- Create: `src/led/led.hpp`, `src/led/led.cpp`
- Delete: `src/led.c`
- Modify: `CMakeLists.txt`

Self-contained (no callers); auto-starts via `SYS_INIT`. Singleton.

- [ ] **Step 1: Create `src/led/led.hpp`**

```cpp
#ifndef LED_LED_HPP_
#define LED_LED_HPP_

namespace led {

class Led {
 public:
  static Led& Instance();
  void Start();
  void Stop();

 private:
  Led() = default;
};

}  // namespace led

#endif  // LED_LED_HPP_
```

- [ ] **Step 2: Create `src/led/led.cpp`** (bodies transcribed verbatim from `led.c`)

```cpp
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

const struct gpio_dt_spec led_spec = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

void led_blink_timer_cb(struct k_timer* timer_id) {
  gpio_pin_toggle_dt(&led_spec);
}
K_TIMER_DEFINE(led_blink_timer, led_blink_timer_cb, NULL);

}  // namespace

namespace led {

Led& Led::Instance() {
  static Led instance;
  return instance;
}

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

static int led_app_add(const struct device* arg) {
  ARG_UNUSED(arg);
  led::Led::Instance().Start();
  return 0;
}
SYS_INIT(led_app_add, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 3: Delete old file, update `CMakeLists.txt`**

```bash
cd /Users/litao/Developer/zpsu_mon
git rm src/led.c
```
In `CMakeLists.txt`: add `${PROJECT_SOURCE_DIR}/src/led` to `include_directories(...)`. In `app_SRCS`: remove `${PROJECT_SOURCE_DIR}/src/led.c`; add `${PROJECT_SOURCE_DIR}/src/led/led.cpp`.

- [ ] **Step 4: Build**

Run the standard build command. Expected: compiles + links.

- [ ] **Step 5: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/led/led.hpp src/led/led.cpp CMakeLists.txt
git commit -m "refactor(led): convert led.c to led::Led

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 8: Final verification

**Files:** none (verification + reporting).

- [ ] **Step 1: Clean rebuild + memory report**

```bash
cd /Users/litao/Developer/zpsu_mon && \
  export PATH="/Users/litao/anaconda3/bin:$PATH" && \
  export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr && \
  ninja -C build_lcd2 clean >/dev/null 2>&1 && ninja -C build_lcd2
```
Expected: clean from-scratch build links (`Linking CXX executable`), `Wrote ... zephyr.uf2`. Record final FLASH/RAM "Used Size"; confirm RAM < 100%.

- [ ] **Step 2: Confirm the old C symbols are gone from compiled sources**

```bash
cd /Users/litao/Developer/zpsu_mon
grep -rnE "watchface_app_start|watchface_set_|watchface_show|watchface_remove|clock_get_time|notification_manager_|display_control_init|display_control_power_on|display_control_set_brightness|\blvgl_update\b|led_app_|pwm_rgb_led_init|run_init_work" \
  --include="*.c" --include="*.cpp" --include="*.h" --include="*.hpp" src \
  | grep -vE "watchface_anim\.c"
```
Expected: NO matches. (The `watchface_app_on_*` glue hooks and the `psuctrl_data_chan`/zbus names are intentionally retained but do not match these patterns. The only allowed residual is in the untracked, uncompiled `watchface_anim.c`.)

- [ ] **Step 3: Confirm only intended C files remain**

```bash
cd /Users/litao/Developer/zpsu_mon
grep -oE 'src/[^ ]+\.c\b' CMakeLists.txt | sort
```
Expected C (`.c`) entries in `app_SRCS`: only the generated UI (`ui/ui.c`, `ui/ui_helpers.c`, `ui/screens/ui_Screen1.c`, `ui/fonts/*.c`), `psu/psu_channel.c`, and `app/watchface_app_glue.c`. Everything else is now `.cpp`.

- [ ] **Step 4: Hardware handoff**

Tell the developer to flash `build_lcd2/zephyr/zephyr.uf2` to the RP2040 (BOOTSEL) and confirm: the watchface renders and the PSU panel (volts/amps/watts/energy) updates, the ON/OFF and CV/CC labels toggle, the CC value adjusts with UP/DOWN, the display powers on, and the status LED blinks at 1 Hz.

---

## Self-Review Notes (author check)

- **Spec coverage:** all 7 modules from the spec table have a task (Tasks 1–7); generated UI / fonts / `psu_channel.c` / `watchface_anim.c` are untouched; verification (§8) → Task 8.
- **Ordering deviation from spec:** the spec said leaf→main; this plan uses caller-first (main→watchface_app→leaves) because leaf-first would require throwaway `extern "C"` shims to keep a still-C caller linking against a converted C++ leaf. Caller-first keeps every commit green with no shim code. Documented in the header.
- **Type/name consistency:** `app::App`, `app::WatchfaceApp` (+ `watchface_app_on_*` hooks, `set_running`/`running()`), `ui::Watchface` (PascalCase `Set*`/`Show`/`Remove`/`SetEnergyPanel`), `timekeeping::Clock` (`GetTime`), `notify::NotificationManager` (`GetNum`, members `*_`), `display::DisplayControl` (`Init`/`PowerOn`/`SetBrightness`/`LvglUpdate`), `led::Led`. Caller-update tables in Tasks 3–6 match these exactly.
- **C-macro boundary:** `ZBUS_LISTENER_DEFINE`/`BT_CONN_CB_DEFINE` isolated in `watchface_app_glue.c`; `SYS_INIT`/`K_WORK*`/`K_TIMER_DEFINE` kept in `.cpp` (proven to compile as C++ in `psu_service.cpp`).
- **Implicit-declaration trap:** `watchface_app.cpp` must `#include <display_control.h>` for `lvgl_update` (the original `.c` relied on an implicit declaration, which is a hard error in C++); included in Task 2, switched to the `.hpp` in Task 6.
