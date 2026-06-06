# PSU Module C++ Refactor Design

- Date: 2026-06-06
- Module: `psu_ctrl` (PSU / DPS1200 power-supply control)
- Goal: Refactor the PSU control module from procedural C into object-oriented
  C++ using the Google C++ Style Guide, and clean up related non-standard
  naming and dead code in the repository.

## 1. Background and Motivation

The existing `src/psu_ctrl.c` has the following problems:

- **Inconsistent naming**: function names mix four styles —
  `PSUCtrl_init` (Pascal prefix), `psuctrl_init` (snake), `PSUCtrl_ONOFF`
  (all caps), `forceFanRPM` (camelCase).
- **Global mutable state**: `i2cdev_`, `address_`, `mutex_`, etc. live as
  file-scope statics with no encapsulation.
- **Manual lock management**: every error branch must call `k_mutex_unlock`
  by hand, which is easy to miss.
- **Magic numbers**: register addresses (`0x21` / `0x1f`) and scale factors
  (`/265`, `/64`) are scattered through the code.
- **Dead code**: the `lastReg_/minReg_/maxReg_` register caches are allocated
  but never used; `PSUCtrl_deinit` is never called.
- **UI/logic coupling**: PSU register operations and LVGL label updates are
  mixed in the same functions.

## 2. Confirmed Decisions

| Decision | Choice |
|---|---|
| C++ runtime | Full runtime (full libcpp + exceptions + RTTI) |
| Logic / UI | Separated: `PsuController` class touches no LVGL/zbus; glue in adapter |
| File layout | Split: `psu_controller.*` (class) + `psu_service.*` (adapter) |
| Naming convention | Google C++ Style |
| Refactor scope | Professional, layered: full refactor of the PSU module + dead-code removal; generated code and upstream code stay behind a boundary, untouched |
| LVGL callbacks | Renamed (with a SquareLine sync checklist) |

## 3. Architecture

```
┌─ src/psu/psu_service.hpp / .cpp ──── Adapter layer (C API, extern "C")
│   · Holds a single psu::PsuController (Meyers singleton)
│   · zbus channel definition + 500ms periodic work + energy accumulation + publish
│   · LVGL event callbacks: call controller methods -> update labels
│   · psu_service_init() + SYS_INIT entry
│
└─ src/psu/psu_controller.hpp / .cpp ── Pure C++ class
    · I2C transport + DPS1200 protocol (checksum) + high-level ops + measurement
    · No LVGL, no zbus dependency -> independently buildable/reusable/testable
```

Rationale: zbus is a data-transport/integration concern and LVGL is UI; neither
belongs to "PSU control logic", so `PsuController` stays clean and all glue is
concentrated in the service layer.

## 4. `psu::PsuController` Class Interface

```cpp
namespace psu {

struct Measurement { float volts; float amps; float watts; };
enum class OutputMode { kConstantVoltage, kConstantCurrent };

class PsuController {
 public:
  PsuController(const device* i2c_dev, uint8_t address_index);
  bool IsReady() const;

  // DPS1200 register level (with two's-complement checksum)
  bool ReadRegister(uint8_t reg, int* value);
  bool WriteRegister(uint8_t reg, int value);

  // High-level operations (preserve existing register/scaling logic)
  bool ReadMeasurement(Measurement* out);   // regs 7/8/9 + OVP scaling
  bool SetFanRpm(int rpm);                   // 0x20
  bool ToggleOutput(bool* enabled_out);      // 0x21 bit15
  bool ToggleMode(OutputMode* mode_out);     // 0x21 bit0
  bool GetConstantCurrent(float* amps_out);  // 0x1f / 265
  bool SetConstantCurrent(float amps);

 private:
  bool ReadFrame(uint8_t reg, uint8_t* data, uint32_t count);
  const device* i2c_dev_;
  uint8_t device_address_;     // 0x58 + index
  uint8_t eeprom_address_;     // 0x50 + index
  k_mutex bus_mutex_;
};

}  // namespace psu
```

### Design details

- **Named constants** (replacing magic numbers, Google `kName` style):
  `kRegVolts=7`, `kRegAmps=8`, `kRegWatts=9`, `kRegConstantCurrent=0x1f`,
  `kRegStatus=0x21`, `kRegFanRpm=0x20`, `kCurrentScale=265.0f`,
  `kAmpsScale=64.0f`, `kVoltsScaleBase=256`, `kOutputEnableBit=1u<<15`,
  `kConstantCurrentBit=1u<<0`.
- **Error handling**: uniformly return `bool` (success/failure) + out-pointer.
  I2C failure is routine; the hot path does not throw.
- **RAII lock**: a private `ScopedLock` in the `.cpp` that calls
  `k_mutex_lock(…, K_MSEC(100))` in its constructor and unlocks in its
  destructor; `locked()` reflects the lock result, eliminating manual-unlock
  omissions.
- **Singleton timing**: the adapter uses a Meyers singleton (function-local
  `static`) so construction happens in `psu_service_init()` (APPLICATION level,
  I2C already ready), not before the kernel starts.

## 5. C Adapter Interface and Callback Renames

The `psu_service.hpp` C interface (`extern "C"`). LVGL callbacks follow LVGL's
snake_case `_event_cb` convention:

| Purpose | Old name | New name |
|---|---|---|
| SYS_INIT entry | `psuctrl_init` | `psu_service_init` |
| Initial label sync | `PSUCtrl_UI_Init` | `psu_ui_init` |
| On/off callback | `PSUCtrl_ONOFF` | `psu_power_toggle_event_cb` |
| CV/CC callback | `PSUCtrl_CVCC` | `psu_mode_toggle_event_cb` |
| Set-CC callback | `PSUCtrl_Set_CC` | `psu_current_set_event_cb` |
| Fan speed | `PSUCtrl_forceFanRPM` | `psu_set_fan_rpm` |

### SquareLine sync checklist (must be applied in the SquareLine project, otherwise the next export reverts it)

- `ui_LabelOnOff` CLICKED event -> `psu_power_toggle_event_cb`
- `ui_LabelCVCC` CLICKED event -> `psu_mode_toggle_event_cb`
- `ui_lblPsuCc` KEY event -> `psu_current_set_event_cb`

## 6. Boundary: Left Untouched

- **zbus contract keeps its names**: `psuctrl_data_chan` /
  `struct psuctrl_data_event` / `watchface_psuctrl_event` are not renamed —
  they are referenced by the ZSWatch upstream `watchface_app.c`, and renaming
  would touch upstream files.
- **Generated code not renamed**: `ui.c`, `ui_helpers.*`, `ui_Screen1.*`,
  `ui_events.h`, `fonts/*` (SquareLine / LVGL generated; renames would be
  overwritten on export).
- **ZSWatch upstream code not renamed**: `display_control.c`,
  `notification_manager.c`, `watchface_app.c`, `zsw_charger.h`, `zsw_cpu_freq.h`
  (renames would break upstream merges).

## 7. Repository Hygiene and Dead-Code Cleanup

- Delete zero-reference dead headers (re-confirm before deletion):
  `ble_aoa.h`, `ble_transport.h`, `magnetometer.h`, `gpio_debug.h`.
- Delete in-module dead code: the `lastReg_/minReg_/maxReg_` caches and
  `PSUCtrl_deinit`.

## 8. Behavior Preservation (pure structural refactor)

The I2C protocol, checksum algorithm, register addresses, scale factors
(voltage `/(OVP_SCALE_FACTOR*256)`, current `/64`, CC `*265`), the 500ms poll
period, the zbus event contract, and the `OVP_SCALE_FACTOR` compile-time macro
are all unchanged.

## 9. Affected Non-Generated / Non-Upstream Files (minor edits allowed)

- `CMakeLists.txt`: remove `psu_ctrl.c`, add `src/psu/psu_controller.cpp` and
  `src/psu/psu_service.cpp`; add `src/psu` to `include_directories`.
- `src/main.c`: update `#include` to the new header; clean up commented
  references to old symbol names.
- `src/ui/watchface_ui.c`: `PSUCtrl_UI_Init()` -> `psu_ui_init()`, update include.
- `prj.conf`: add `CONFIG_CPP=y`, `CONFIG_STD_CPP20=y`,
  `CONFIG_REQUIRES_FULL_LIBCPP=y`, `CONFIG_CPP_EXCEPTIONS=y`,
  `CONFIG_CPP_RTTI=y`.

## 10. Verification

This project has no automated test framework (bare-metal embedded).
Acceptance criteria:

1. **Compiles and links**: `ninja -C build_lcd2` (Anaconda toolchain, board
   `rpi_pico`). Confirm in particular that the `extern "C"` boundary has no
   name-mangling link errors.
2. **Still fits in memory**: WARNING — full libcpp + exceptions + RTTI is the
   biggest risk at the current 85% RAM usage. Check the RAM/Flash report after
   building; if it overflows, return to this design to discuss what to trim
   (most likely disabling `CONFIG_CPP_EXCEPTIONS`).
3. **Hardware check**: the developer flashes `build_lcd2/zephyr/zephyr.uf2` to
   the RP2040 to confirm real behavior.

## 11. Risks

- **Memory overflow**: see §10.2. Mitigation: per-item reversible Kconfig
  options; dead-code removal also reclaims some RAM.
- **SquareLine overwrite**: the callback renames depend on the developer
  syncing the SquareLine project (§5). Until synced, UI events may bind to the
  old names and fail to link — code and project must be kept in correspondence
  within the same change.
