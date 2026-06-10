# Software Upgrade over USB — Design

> **STATUS (superseded): the MCUboot serial-recovery design below was built then ABANDONED.**
> On-hardware testing showed MCUboot serial recovery is unworkable on RP2040 (the SoC
> `rom_bootloader.c` hijacks the retention boot-mode and jumps to the USB mask ROM before
> MCUboot recovery runs; `sys_reboot`/SYSRESETREQ is also unreliable). The shipped design is
> **Path A** (implemented in `src/dfu/`, opt-in `CONFIG_APP_USB_DFU`): the app's `dfu` / UDP
> `DFU` / A+Y trigger calls the RP2040 bootrom `reset_usb_boot()` to drop straight into the
> USB BOOTSEL bootloader; upgrade via `picotool load` / UF2 drag. No MCUboot, no repartition,
> no signing; works on the stock build. **See §10 for the rationale, the pivot, and findings.**
> The sections below are retained for the design history (why USB-DFU, the flash-budget
> analysis, the trigger entry points — all still valid).

- Date: 2026-06-08
- Scope: All build variants (lcd1–4, wifi, ap) on `rpi_pico` (RP2040) and `rpi_pico2` (RP2350)
- Goal: Add a software-triggered firmware upgrade path over USB — no BOOTSEL button press
  for the common case — using the MCUboot bootloader in single-application-slot mode with
  serial recovery over USB-CDC. Uniform mechanism across every build, opt-in via a new
  `--sysbuild` / `*/mcuboot` build target, leaving the existing bootloader-less builds intact.

## 1. Background

The project is a Zephyr v4.4.0 PSU monitor / smartwatch on RP2040 (`rpi_pico`) and RP2350
(`rpi_pico2`), with opt-in WiFi STA and SoftAP variants on `rpi_pico/rp2040/w`. Today the only
way to update firmware is the RP2040 ROM path: hold BOOTSEL, drag a `.uf2` onto the
`RPI-RP2` mass-storage device. There is no in-application or over-the-wire upgrade.

The user wants a **software-triggered USB upgrade**: trigger from the running app, then push
new firmware over the USB cable with a host tool — no physical button for the normal case.

### Hard constraint discovered during brainstorming

| Build | `zephyr.bin` size |
|---|---|
| Non-WiFi watch (build_lcd2) | ~620 KB |
| PIO display (build_pio) | ~612 KB |
| WiFi (build_wifi) | ~1.1 MB |
| SoftAP (build_ap) | ~1.1 MB |

Zephyr v4.4.0 already ships MCUboot board variants for exactly our targets
(`rpi_pico/rp2040/mcuboot`, `rpi_pico/rp2040/w/mcuboot`, `rpi_pico2/rp2350a/m33/mcuboot`,
`.../w/mcuboot`) and a stock 2 MB **dual-slot** sysbuild layout
(`dts/vendor/raspberrypi/partitions_2M_sysbuild.dtsi`):

| Partition | Size |
|---|---|
| second_stage_bootloader | 256 B |
| mcuboot | ~63.5 KB |
| slot0 (image-0) | **832 KB** |
| slot1 (image-1) | **832 KB** |
| storage | 320 KB |

The WiFi/AP image (~1.1 MB) **does not fit** in an 832 KB slot, and `2 × 1.1 MB = 2.2 MB`
**cannot** form a dual-slot layout in 2 MB of flash at all. The CYW43439 firmware blob is
linked into the image, which is what makes the WiFi build large.

Therefore a dual-slot A/B scheme is impossible for the WiFi build. To keep **one uniform
mechanism across every build**, we use **single-application-slot** mode (one slot, overwrite
in place). The WiFi image fits comfortably in a single ~1.84 MB slot; the smaller non-WiFi
images fit trivially.

## 2. Confirmed decisions

| Decision | Choice |
|---|---|
| Upgrade channel | USB software-triggered DFU (not wireless OTA, not improved-UF2) |
| Scope | All build variants, one uniform mechanism |
| Bootloader | MCUboot, **single application slot** (`CONFIG_SINGLE_APPLICATION_SLOT`) |
| Transport | MCUboot **serial recovery** over **USB-CDC ACM** (SMP protocol) |
| Rollback | None (overwrite-in-place). BOOTSEL ROM is the always-available recovery net |
| Trigger entry points | USB-CDC shell `dfu` command; UDP `DFU` (AP build); on-screen UI button |
| Trigger mechanism | App sets a retention boot-mode flag, then `sys_reboot()`; MCUboot stays in serial recovery |
| Opt-in | New `--sysbuild -b <board>/mcuboot` targets; default bootloader-less builds unchanged |
| First install | One-time BOOTSEL → drag merged `.uf2` (partition map changes); later upgrades are USB-only |
| Host tool | `mcumgr` CLI over serial |
| Signing key | MCUboot bundled dev key for now; production should swap in a private key |

## 3. Architecture

```
App running ──trigger──▶ set boot-mode flag (retained RAM) ──▶ sys_reboot()
                                                                   │
MCUboot boot ──flag set?──▶ stay in serial recovery; SMP server on USB-CDC ACM
                                                                   │
Host: mcumgr image upload zephyr.signed.bin ──▶ overwrite the single slot ──▶ boot new image
       mcumgr reset
```

- **Single application slot**: there is only `slot0`. The running app cannot overwrite itself,
  so the SMP upload runs **inside MCUboot** (serial recovery), not in the app. This is the
  documented MCUboot mode for flash-constrained devices.
- **Software trigger**: the app does not need an SMP server. It only needs to request
  bootloader entry. It writes a retention boot-mode value and reboots; MCUboot, built with
  `CONFIG_BOOT_SERIAL_BOOT_MODE`, reads that value and stays in serial recovery instead of
  booting the (now stale) app.
- **No rollback**: overwrite-in-place means a failed/interrupted upload can leave no valid
  image. Recovery is the RP2040/RP2350 BOOTSEL ROM bootloader, which is in mask ROM and
  cannot be bricked — drag the merged `.uf2` to recover.

## 4. Flash partition layout (custom single-slot, 2 MB)

The stock 2M sysbuild layout is dual-slot, so we define our own single-slot layout. Indicative
sizing (final numbers settled in the plan; boot partition size depends on Risk 1 below):

| Partition | Offset | Size | Notes |
|---|---|---|---|
| second_stage_bootloader | 0x0 | 256 B | RP2040 boot2 |
| mcuboot | 0x100 | ~96 KB | headroom for USB-CDC + SMP |
| slot0 (image-0, the only app slot) | ~0x18100 | ~1.84 MB | 1.1 MB WiFi image fits with room |
| storage (NVS) | top | 64 KB | existing WiFi/AP credential store |

For `rpi_pico2` (RP2350, 4 MB) we keep single-slot for uniformity. The 4 MB part has room for
a dual-slot A/B layout, which is recorded as a future option if automatic rollback is wanted
there — out of scope for this design.

## 5. Components

### 5.1 New module `src/dfu/`
A single shared entry point, plus the boot-mode plumbing:

```
dfu.c / dfu.h
  void dfu_enter_recovery(void);   // set retention boot-mode flag, then sys_reboot()
```

All three triggers call `dfu_enter_recovery()`. Compiled into every upgrade-capable build.

### 5.2 Trigger entry points
- **Shell `dfu` command** — registered with `SHELL_CMD_REGISTER`. `CONFIG_SHELL=y` is already
  present in `wifi.conf` / `ap.conf`, so this works on the WiFi/AP builds. The base `prj.conf`
  has no shell, so non-WiFi builds rely on the UI button instead (we do **not** force a shell
  into the non-WiFi builds).
- **UDP `DFU` command (AP build)** — add a `DFU` branch to `handle_cmd()` in `src/net/psu_udp.c`
  (the existing `STATUS|ON|OFF|...|SETAP` dispatcher). Reply `OK DFU (rebooting)` then call
  `dfu_enter_recovery()`. Available on the AP build only.
- **On-screen UI entry** — a "Firmware Upgrade" item that pops an `lv_msgbox` confirmation
  (guard against accidental taps; `CONFIG_LV_USE_MSGBOX=y` is already set) and, on confirm,
  calls `dfu_enter_recovery()`. The action lives in app code; per the project convention the
  generated UI just invokes the hook (kept separate from SquareLine-generated UI).

### 5.3 MCUboot sysbuild config (`sysbuild/mcuboot.conf`)
- `CONFIG_SINGLE_APPLICATION_SLOT=y`
- `CONFIG_BOOT_SERIAL=y`
- `CONFIG_BOOT_SERIAL_CDC_ACM=y` (USB-CDC transport)
- `CONFIG_BOOT_SERIAL_BOOT_MODE=y` (read the retention flag the app sets)
- MCUboot USB device descriptors (VID/PID, product string)

### 5.4 App-side config
- `CONFIG_BOOTLOADER_MCUBOOT=y` (link at slot0 offset, add the MCUboot image header)
- `CONFIG_RETENTION=y` + `CONFIG_RETENTION_BOOT_MODE=y` and a retained-RAM (`noinit`) region
  in the devicetree for the boot-mode value
- App changes are deliberately thin: no SMP server in the app, only the trigger + boot-mode set.

## 6. Build and flash flow

- Upgrade-capable build uses sysbuild and the `*/mcuboot` board, e.g.:
  ```
  west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_wifi_ota \
      -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf
  ```
  Sysbuild produces a merged `.uf2` (MCUboot + signed app) and a `zephyr.signed.bin` for OTA.
- **First install (one-time):** BOOTSEL → drag the merged `.uf2`. Required because the
  partition map changes when moving to MCUboot.
- **Subsequent upgrades (USB-only, no button):**
  1. Trigger from the device (shell `dfu` / UDP `DFU` / UI button) → it reboots into recovery.
  2. `mcumgr --conntype serial --connstring "dev=<port>,baud=115200" image upload zephyr.signed.bin`
  3. `mcumgr ... reset` → MCUboot boots the new image.
- README: add build-matrix lines only (per the project's minimal-README convention); detail
  stays in this spec.

## 7. Risks and mitigations

1. **MCUboot + USB-CDC + SMP footprint** (highest risk): must fit the boot partition and
   enumerate a CDC ACM port on RP2040. *Mitigation:* the plan starts with a minimal build
   spike to confirm size and enumeration. If it does not fit ~96 KB, enlarge the boot
   partition (single-slot leaves slack). If USB-CDC is impractical, fall back to UART
   (GPIO0/1) serial recovery, accepting that it needs a USB-UART adapter.
2. **Retention flag surviving warm reset on RP2040**: `sys_reboot()` is a soft reset that
   should not clear SRAM, but this must be verified on hardware. *Mitigation fallback:*
   `CONFIG_BOOT_SERIAL_ENTRANCE_GPIO` (hold a button at reset) as an alternate entry.
3. **Signing key**: dev key is fine for development; production must use a private key. Noted,
   not solved here.
4. **USB-CDC coexistence**: the app uses USB-CDC for console/logs and MCUboot uses USB-CDC for
   recovery, but never simultaneously (one runs at a time), so the host simply sees the port
   re-enumerate at the app↔bootloader transition. No design conflict.

## 8. Testing

- **Host-testable unit test**: a parser test for the new UDP `DFU` command, in the style of the
  existing `cred_parse` tests under `tests/`.
- **On-hardware smoke**: trigger via each of the three entry points → confirm the device enters
  serial recovery → `mcumgr image upload` a rebuilt image with a bumped version string →
  confirm the new version boots → confirm BOOTSEL UF2 still recovers a deliberately-interrupted
  upload.

## 9. Out of scope

- Wireless OTA over WiFi (the 2 MB flash budget rules out dual-slot; single-slot OTA while the
  app runs is not possible, and a WiFi-driven serial-recovery handoff is a larger effort).
- Automatic A/B rollback (incompatible with the uniform single-slot choice on 2 MB).
- Dual-slot on RP2350/4 MB (recorded as a future option only).

## 10. Status / follow-on

Implemented for **RP2040** (the size-constraining, debug-probe-wired target) per
`docs/superpowers/plans/2026-06-09-usb-dfu-mcuboot-upgrade.md`: sysbuild MCUboot single-slot
+ USB-CDC serial recovery, boot-mode-retention software trigger (shell `dfu`, UDP `DFU`,
A+Y button hold), all host-build-verified; on-hardware flash/`mcumgr` verification is the
operator's gate.

**RP2350 (`rpi_pico2/rp2350a/m33/mcuboot`, 4 MB) follow-on** — reuses the *identical*
mechanism; the only delta is a sibling overlay `boards/rpi_pico2/dfu_singleslot.dtsi`
(a 4 MB single-slot map + `#include <raspberrypi/rp2350-boot-mode-retention.dtsi>`) and a
namespaced sysbuild build for that board. No on-metal coverage yet.

**Build-integration note:** a sysbuild child image does not see `BOARD`/`CONFIG_*`/`EXTRA_CONF_FILE`
before `find_package(Zephyr)`, so the app's overlays/conf are passed namespaced
(`-Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE=…`, `-Dzpsu_mon_EXTRA_CONF_FILE=…`) rather than via the
pre-`find_package` CMake gate; `CMakeLists.txt` only gains a post-`find_package`
`if(CONFIG_BOOTLOADER_MCUBOOT)` block to compile `src/dfu/`.
