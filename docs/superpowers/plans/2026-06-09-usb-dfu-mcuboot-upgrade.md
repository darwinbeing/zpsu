# Software Upgrade over USB (MCUboot Serial Recovery) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a software-triggered USB firmware upgrade — trigger from the running app (shell / UDP / on-device buttons), then push a new image over USB with `mcumgr` — using MCUboot in single-application-slot mode with serial recovery over USB-CDC, opt-in via a new `--sysbuild -b <board>/mcuboot` build that leaves every default bootloader-less build untouched.

**Architecture:** A new sysbuild MCUboot child image (`sysbuild.conf` + `sysbuild/mcuboot.{conf,overlay}`) boots before the app. A custom single-slot 2 MB flash map (`boards/rpi_pico/dfu_singleslot.dtsi`), shared by both images, replaces the board variant's stock dual-slot layout — one ~1.84 MB app slot (the ~1.1 MB WiFi image cannot fit an 832 KB A/B slot). MCUboot runs an SMP server over USB-CDC ACM and stays in serial recovery when the app sets a retained-RAM boot-mode byte (Zephyr's `rp2040-boot-mode-retention.dtsi`) and warm-reboots. A thin app module `src/dfu/` provides the single trigger `dfu_enter_recovery()`, called from three entry points (shell `dfu`, UDP `DFU`, a two-button hold). BOOTSEL ROM is the un-brickable recovery net (no rollback).

**Tech Stack:** Zephyr v4.4.0, sysbuild, MCUboot (`SINGLE_APPLICATION_SLOT`, `BOOT_SERIAL_CDC_ACM`, `BOOT_SERIAL_BOOT_MODE`), Zephyr `retention`/`retained_mem`, RP2040 (`rpi_pico/rp2040/w/mcuboot`), USB-CDC ACM, `mcumgr` CLI (host), `input` subsystem (buttons), west/CMake. Pure command-matching logic is Zephyr-free C tested with host `clang` (the `cred_parse` pattern).

**Spec:** `docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md`

---

## Important notes for the implementer

- **This is on-target embedded firmware.** There is no host test harness for the firmware itself. "Verification" per task means: (a) the sysbuild build succeeds and **fits the boot partition / RP2040 RAM**, and (b) **on-hardware** behaviour observed over USB-CDC and `mcumgr`. The one genuine automated test is the **host-side** `dfu_cmd` matcher (Task 3, plain `clang`). Do not invent pytest-style firmware tests.
- **Primary target = `rpi_pico/rp2040/w` (Pico W).** It is the size-constraining variant (WiFi image ~1.1 MB) and the board the user has wired for debugging (Raspberry Pi Debugprobe / CMSIS-DAP + openocd, per project memory). All on-metal tasks are proven there. RP2350 (4 MB) and the non-W RP2040 reuse the same mechanism and are covered by Task 5 (CI/docs) + a sibling overlay; they are **not** the de-risk vehicle.
- **Default builds must stay byte-for-byte unaffected.** Everything here is gated behind `--sysbuild` + the `*/mcuboot` board variant. `sysbuild.conf` is only read when `--sysbuild` is passed; the new CMake overlay block keys off `BOARD MATCHES "mcuboot"`; `dfu.conf` / `src/dfu/` only enter the upgrade build. After **every** task, re-run the default baseline build (`build_lcd2`) and confirm it is unchanged.
- **Branch:** work on `feat/usb-dfu-mcuboot`. The design spec is already committed on `main` (commit `0a63e07`); create the branch from there.
- **Patches still apply.** The upgrade build is still the `rpi_pico/rp2040/w` HAL; the three out-of-tree patches (`st7789v_clear_gram`, `rp2040_flash_ramfunc`, `cyw43439_ap_set_channel`) must be applied to the west workspace exactly as today (`patches/README.md`). `rp2040_flash_ramfunc` is required for the NVS cred store; `cyw43439_ap_set_channel` for the AP build.
- **Spike first.** Task 1 stands up the entire build chain and proves Risk 1 (MCUboot + USB-CDC + SMP **footprint and CDC enumeration on RP2040**) using a *temporary* `WAIT_FOR_DFU` entry — **before** any app C code. If the bootloader does not fit ~96 KB, the only change is enlarging `boot_partition` (the single slot has slack); if USB-CDC is impractical, fall back to UART serial recovery (Risk 1 mitigation in the spec).
- **Sysbuild image names.** Sysbuild puts each image under `build/<image-name>/`. The bootloader is `build/<d>/mcuboot/`; the main app image name is the CMake `project()` name. **Task 1 Step 8 records the real paths via `ls`** — every later task references those recorded paths, not guesses.

---

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `sysbuild.conf` (repo root) | Enable MCUboot in sysbuild; single-application-slot mode | 1 |
| `sysbuild/mcuboot.conf` | MCUboot child-image Kconfig: serial recovery, USB-CDC, boot-mode retention | 1, 2 |
| `sysbuild/mcuboot.overlay` | MCUboot child-image DT: shared single-slot map + CDC-ACM recovery port | 1 |
| `boards/rpi_pico/dfu_singleslot.dtsi` | **Shared** single-slot 2 MB flash map + RP2040 boot-mode retention (app + mcuboot) | 1 |
| `dfu.conf` (repo root) | App-side Kconfig for the upgrade build (MCUboot header, retention, reboot) | 1, 2 |
| `CMakeLists.txt` | Compile `src/dfu/` under `if(CONFIG_BOOTLOADER_MCUBOOT)` (post-`find_package`). **No overlay changes** — app overlays/conf are passed namespaced (`-Dzpsu_mon_*`); see Task 1 Step 6 | 2, 3, 4 |
| `src/dfu/dfu.h` / `dfu.c` | `dfu_enter_recovery()` — set boot-mode, deferred warm reboot; shell `dfu` cmd | 2 |
| `src/dfu/dfu_cmd.h` / `dfu_cmd.c` | Zephyr-free `dfu_cmd_is_trigger()` matcher (host-testable) | 3 |
| `tests/dfu_cmd/test_dfu_cmd.c` | Host `clang` test for the matcher (cred_parse style) | 3 |
| `src/net/psu_udp.c` | `DFU` UDP verb → reply, then `dfu_enter_recovery()` | 3 |
| `src/dfu/dfu_buttons.c` | A+Y 2 s hold → confirm msgbox → `dfu_enter_recovery()` (non-WiFi trigger) | 4 |
| `.github/workflows/firmware.yml` | CI: build the `*/mcuboot` sysbuild variant; publish merged image | 5 |
| `README.md` | Build-matrix line(s) + upgrade how-to pointer | 5 |

---

## Prerequisites (one-time, not a commit)

- [ ] **Branch from the committed design:**

```bash
cd ~/Developer/zpsu_mon
git checkout -b feat/usb-dfu-mcuboot main
```

- [ ] **`mcumgr` CLI on the host** (the upload tool). With Go installed:

```bash
go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest   # puts `mcumgr` in $(go env GOPATH)/bin
mcumgr version
```
Expected: a version prints. If you have no Go toolchain, install a prebuilt `mcumgr` binary instead; any later step that runs `mcumgr` needs it on `$PATH`.

- [ ] **Patches applied to the west workspace** (re-applied after any `west update`):

```bash
cd ~/Developer/zpsu_mon
git -C ~/Developer/zephyrproject/zephyr               apply "$PWD/patches/st7789v_clear_gram.patch"
git -C ~/Developer/zephyrproject/modules/hal/rpi_pico apply "$PWD/patches/rp2040_flash_ramfunc.patch"
git -C ~/Developer/zephyrproject/modules/hal/infineon apply "$PWD/patches/cyw43439_ap_set_channel.patch"
```
Expected: no output (clean apply). If a patch is already applied, `git apply --reverse --check` it first; do not double-apply.

- [ ] **WiFi blob present** (already fetched for the WiFi work; confirm):

```bash
find ~/Developer/zephyrproject -ipath '*hal_infineon*' -iname '*.bin' | grep -i 43439 | head
```
Expected: at least one path. If empty: `cd ~/Developer/zephyrproject && west blobs fetch hal_infineon`.

- [ ] **Default baseline builds** (the regression yardstick; use the conda `zephyr44` env, `ZEPHYR_BASE=~/Developer/zephyrproject/zephyr`, per project memory):

```bash
cd ~/Developer/zpsu_mon
source ~/Developer/zephyrproject/zephyr/zephyr-env.sh
west build -b rpi_pico -d build_lcd2 -p auto -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: builds clean. Record its `zephyr.bin` size (`ls -l build_lcd2/zephyr/zephyr.bin`) — Task-end regression checks compare against it.

---

## Task 1: Build + flash spike — MCUboot single-slot + USB-CDC serial recovery on metal (no app C)

This task stands up the **entire** sysbuild + MCUboot single-slot + USB-CDC serial-recovery chain and proves Risk 1 (footprint + CDC enumeration + an `mcumgr` upload) on hardware, using a **temporary** `WAIT_FOR_DFU` recovery entry. No project C code yet, and no app trigger yet.

**Files:**
- Create: `sysbuild.conf`, `sysbuild/mcuboot.conf`, `sysbuild/mcuboot.overlay`, `boards/rpi_pico/dfu_singleslot.dtsi`, `dfu.conf`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Sysbuild MCUboot mode (`sysbuild.conf`)**

Create `sysbuild.conf` at the repo root:

```ini
# Sysbuild config for the opt-in MCUboot USB-DFU upgrade build. Only read when
# building with `--sysbuild`; the default bootloader-less builds never see this
# file, so they are 100% unaffected. Build e.g.:
#   west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_wifi \
#       -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE="wifi.conf;dfu.conf"
SB_CONFIG_BOOTLOADER_MCUBOOT=y

# Single application slot (overwrite-in-place). The WiFi image (~1.1 MB) cannot
# fit a dual-slot 832 KB A/B layout in 2 MB flash, so every variant uses one big
# slot. Matches CONFIG_SINGLE_APPLICATION_SLOT in sysbuild/mcuboot.conf.
SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y

# Signing: leave the MCUboot bundled RSA-2048 dev key (SB default). PRODUCTION
# MUST swap in a private key via SB_CONFIG_BOOT_SIGNATURE_KEY_FILE. (Spec Risk 3.)
```

- [ ] **Step 2: Shared single-slot flash map (`boards/rpi_pico/dfu_singleslot.dtsi`)**

Create `boards/rpi_pico/dfu_singleslot.dtsi`. This is the **single source of truth** for the flash map and the retained boot-mode byte, included by *both* images (the app via CMake in Step 6, the bootloader via `sysbuild/mcuboot.overlay` in Step 4):

```dts
/*
 * MCUboot USB-DFU upgrade build (RP2040, 2 MB): a single application slot
 * (overwrite-in-place) + the RP2040 boot-mode retention region. Shared by the
 * application image (DTC_OVERLAY_FILE in CMakeLists for any */mcuboot board)
 * and the MCUboot child image (sysbuild/mcuboot.overlay), so both agree on the
 * flash map AND on the retained boot-mode byte the app sets to request recovery.
 *
 * Overrides the board variant's stock dual-slot partitions_2M_sysbuild.dtsi:
 * the WiFi image (~1.1 MB) cannot fit an 832 KB A/B slot, so we use one ~1.84 MB
 * slot. No rollback — the un-brickable BOOTSEL ROM is the recovery net.
 */

/* Shrinks sram0 by 4 B and adds the RetainedMem region + `chosen zephyr,boot-mode`
 * at 0x20000000 (placed so the bootrom stack does not clobber it). */
#include <raspberrypi/rp2040-boot-mode-retention.dtsi>

&flash0 {
	/delete-node/ partitions;

	partitions {
		compatible = "fixed-partitions";
		#address-cells = <0x1>;
		#size-cells = <0x1>;

		second_stage_bootloader: partition@0 {
			label = "second_stage_bootloader";
			reg = <0x0 0x100>;
			read-only;
		};

		/* MCUboot + USB device stack + CDC-ACM + SMP serial recovery.
		 * ~96 KB. If Task 1 shows the bootloader does not fit, ENLARGE this
		 * (push slot0_partition up to 0x20000 = 128 KB boot); the single slot
		 * has slack to give. */
		boot_partition: partition@100 {
			label = "mcuboot";
			reg = <0x100 0x17f00>;
		};

		/* The only application slot (image-0): ~1.84 MB. */
		slot0_partition: partition@18000 {
			label = "image-0";
			reg = <0x18000 0x1d8000>;
		};

		/* NVS store for runtime WiFi/AP credentials — same 64 KB @0x1f0000
		 * as boards/pico_w_storage.overlay, so creds map identically. Unused
		 * (but harmless) on the non-WiFi upgrade build. */
		storage_partition: partition@1f0000 {
			label = "storage";
			reg = <0x1f0000 DT_SIZE_K(64)>;
		};
	};
};
```

Offsets check: `0x18000 + 0x1d8000 = 0x1f0000`; `0x1f0000 + 0x10000 = 0x200000` (2 MB). `slot0` starts on a 4 KB sector boundary.

- [ ] **Step 3: MCUboot child-image Kconfig (`sysbuild/mcuboot.conf`)**

Create `sysbuild/mcuboot.conf`. Note the **temporary** `WAIT_FOR_DFU` block — it lets the spike reach recovery without an app trigger; Task 2 replaces it with the retention boot-mode path.

```ini
# MCUboot child-image config for the USB-DFU upgrade build (applied to the
# `mcuboot` image by sysbuild). Single application slot + serial recovery (SMP)
# over a USB-CDC ACM port.

# Single slot (matches SB_CONFIG_MCUBOOT_MODE_SINGLE_APP in sysbuild.conf).
CONFIG_SINGLE_APPLICATION_SLOT=y

# Serial recovery over USB-CDC ACM (the mcumgr SMP transport). Mirrors
# bootloader/mcuboot/boot/zephyr/usb_cdc_acm_recovery.conf.
CONFIG_MCUBOOT_SERIAL=y
CONFIG_BOOT_SERIAL_CDC_ACM=y
CONFIG_UART_CONSOLE=n

# The board enables GPIO, so BOOT_SERIAL_ENTRANCE_GPIO defaults y and would
# require an `mcuboot_button0` DT alias (boot/zephyr/io.c #errors without it).
# We enter recovery via WAIT_FOR_DFU (spike) / the retention boot-mode (Task 2),
# not a button held at reset, so turn the GPIO entrance off.
CONFIG_BOOT_SERIAL_ENTRANCE_GPIO=n

# USB device stack + identity shown while in recovery. Distinct product string
# so the host can tell bootloader-recovery apart from the running app's console.
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
CONFIG_USB_DEVICE_VID=0x2e8a
CONFIG_USB_DEVICE_PID=0x000a
CONFIG_USB_DEVICE_PRODUCT="Pico MCUboot DFU"
CONFIG_USB_DEVICE_MANUFACTURER="Raspberry Pi"

# --- TEMPORARY (Task 1 spike only): wait 5 s at every boot for an mcumgr
# connection before booting the app, so we can reach recovery without the app
# trigger. Task 2 DELETES these three lines and adds the retention boot-mode. ---
CONFIG_BOOT_SERIAL_WAIT_FOR_DFU=y
CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=5000
```

- [ ] **Step 4: MCUboot child-image DT overlay (`sysbuild/mcuboot.overlay`)**

Create `sysbuild/mcuboot.overlay`. It pulls in the shared map and exposes the CDC-ACM recovery port (`&zephyr_udc0` is defined by the board in `rpi_pico-common.dtsi`):

```dts
/*
 * DT overlay applied to the MCUboot child image by sysbuild. Shares the
 * single-slot flash map + boot-mode retention with the app
 * (boards/rpi_pico/dfu_singleslot.dtsi), and exposes a USB-CDC ACM port for the
 * serial-recovery SMP transport (mirrors mcuboot's usb_cdc_acm.overlay).
 *
 * The #include is relative to THIS file's directory (C preprocessor rule):
 * sysbuild/ -> ../boards/rpi_pico/.
 */
#include "../boards/rpi_pico/dfu_singleslot.dtsi"

&zephyr_udc0 {
	cdc_acm_uart0 {
		compatible = "zephyr,cdc-acm-uart";
	};
};
```

- [ ] **Step 5: Minimal app-side config (`dfu.conf`)**

Create `dfu.conf` at the repo root. Task 2 adds the retention keys here; Task 1 needs only the MCUboot header so the app lands in slot0 and is signed:

```ini
# --- App-side config for the MCUboot USB-DFU upgrade build (opt-in) ---
# Add to EXTRA_CONF_FILE ONLY for the --sysbuild */mcuboot upgrade builds, e.g.
#   -DEXTRA_CONF_FILE="wifi.conf;dfu.conf"   (WiFi)   or   -DEXTRA_CONF_FILE=dfu.conf (non-WiFi)
# Default (bootloader-less) builds never include this file.

# Link the app into slot0 with an MCUboot image header (sysbuild signs it with
# imgtool). Sysbuild also forces this via SB_CONFIG_BOOTLOADER_MCUBOOT; set here
# too so the intent is explicit at the app level.
CONFIG_BOOTLOADER_MCUBOOT=y
```

- [ ] **Step 6: No CMakeLists changes — pass the app-image overlays/conf namespaced (sysbuild gotcha)**

> **VALIDATED FINDING (Task 1 spike).** Do **not** edit `CMakeLists.txt` for the app overlays. This project selects `DTC_OVERLAY_FILE` *before* `find_package(Zephyr)` using `BOARD`/`CONFIG_*`/`EXTRA_CONF_FILE` as plain CMake vars. That works for a standalone `west build` but **NOT for a sysbuild child image** — those variables are empty at the top of `CMakeLists.txt` when configured under sysbuild, so none of the pre-`find_package` overlay/conf gates fire (confirmed by inspecting the child's generated config). Consequences that make CMakeLists edits pointless here: the pre-`find_package` storage-overlay gate also doesn't fire under sysbuild (no double-`storage_partition` to guard against), and a `BOARD MATCHES "mcuboot"` overlay gate would never trigger either.

Instead, the upgrade build passes everything to the app image **namespaced** with `-D<image>_…` (the image name is `zpsu_mon`). Sysbuild forwards these to the app child, where Zephyr's boilerplate applies `EXTRA_DTC_OVERLAY_FILE` / `EXTRA_CONF_FILE` *after* `find_package` (so they work). This is the entire app-side overlay/conf mechanism for the upgrade build — see Step 7. `CMakeLists.txt` stays byte-for-byte unchanged, so the default builds are provably untouched.

(The mcuboot **child image** still gets the single-slot map via `sysbuild/mcuboot.overlay` from Step 4 — that path is unaffected. Only the **app** image needs the namespaced args.)

- [ ] **Step 7: Build the upgrade image (sysbuild)**

Activate the conda `zephyr44` env (Python ≥3.12) and export the toolchain vars first — `zephyr-env.sh` does **not** set them:

```bash
cd ~/Developer/zpsu_mon
source ~/anaconda3/etc/profile.d/conda.sh && conda activate zephyr44
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=~/Developer/zephyrproject/arm-gnu-toolchain-13.2.Rel1-darwin-x86_64-arm-none-eabi
source ~/Developer/zephyrproject/zephyr/zephyr-env.sh

# App-image overlays/conf are NAMESPACED (-Dzpsu_mon_*) — see Step 6. The bare
# -DCONFIG_.../-DEXTRA_CONF_FILE would go to sysbuild, not the app child.
west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_wifi -p always -- \
    -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
    -Dzpsu_mon_EXTRA_CONF_FILE="wifi.conf;dfu.conf" \
    -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi"
```
If `imgtool` signing errors on a missing module, `pip install cryptography intelhex cbor` into the env (one-time).

**Validated result (Task 1 spike, host build):** both images build. **MCUboot = 50,360 B vs the 0x17f00 = 98,048 B boot partition → fits with 48% headroom (Risk-1 footprint closed).** App image = 70.6% of slot0 flash (signed 1,364,752 B / 1,933,312 B) and 84% RAM. The generated DT of **both** images shows `slot0_partition @0x18000 size 0x1d8000` and the `RetainedMem`/`boot_mode` region — they agree. If a future change overflows the boot partition, enlarge it in `dfu_singleslot.dtsi` (e.g. `boot_partition reg = <0x100 0x1ff00>`, `slot0_partition partition@20000 reg = <0x20000 0x1d0000>`) and rebuild.

- [ ] **Step 8: Record the real artifact paths**

```bash
ls build_dfu_wifi/mcuboot/zephyr/zephyr.{bin,hex,uf2}
ls build_dfu_wifi/zpsu_mon/zephyr/zephyr.signed.{bin,hex}
```
**Validated paths (Task 1 spike):** the bootloader image dir is `build_dfu_wifi/mcuboot/` and the app image dir is `build_dfu_wifi/zpsu_mon/` (the CMake `project()` name). The `mcumgr` upload target is `build_dfu_wifi/zpsu_mon/zephyr/zephyr.signed.bin`. Sysbuild does **not** emit a top-level `merged.hex` for RP2040 (per-image hex/uf2 only) — first install flashes both images (Step 9).

- [ ] **Step 9: First install on hardware**

Use whichever is wired. **Debug probe (cleanest — no BOOTSEL):**
```bash
west flash -d build_dfu_wifi          # sysbuild flashes BOTH images (mcuboot + signed app) over openocd/CMSIS-DAP
```
**Or BOOTSEL drag** (two drops; the board re-enumerates between them — no extra tooling): drag `build_dfu_wifi/mcuboot/zephyr/zephyr.uf2`, then a uf2 of the **signed** app. The signed app is emitted as `.bin`/`.hex` (no signed uf2), so convert it: `picotool uf2 convert build_dfu_wifi/zpsu_mon/zephyr/zephyr.signed.hex zpsu_app_signed.uf2` and drag that.

Expected: after reset, MCUboot's 5 s `WAIT_FOR_DFU` window elapses, then the watchface app boots normally (the WiFi build over USB-CDC). If it boots straight to the app with no upgrade port, the spike still validated the boot chain — continue to Step 10 to catch the window.

- [ ] **Step 10: Prove serial recovery + an `mcumgr` upload (Risk 1)**

Reset the board and **within the 5 s window** identify and talk to the recovery CDC port:
```bash
# macOS: the MCUboot CDC port appears as a new /dev/tty.usbmodem* right after reset.
PORT=$(ls /dev/tty.usbmodem* | head -1)
mcumgr --conntype serial --connstring "dev=$PORT,baud=115200" image list
```
Expected: `mcumgr` prints the slot-0 image info (hash/version) — proves CDC enumeration + SMP. Then upload + boot a rebuilt image:
```bash
# Bump the app version so the new image is visibly different, rebuild, re-enter recovery, upload.
west build -d build_dfu_wifi -- -Dzpsu_mon_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\"1.2.0\"
mcumgr --conntype serial --connstring "dev=$PORT,baud=115200" image upload \
    build_dfu_wifi/zpsu_mon/zephyr/zephyr.signed.bin
mcumgr --conntype serial --connstring "dev=$PORT,baud=115200" reset
```
Expected: upload reaches 100 %, `reset` reboots, the app runs the new image. This **closes Risk 1** (footprint + CDC + upload all work on RP2040). If CDC recovery proves impractical here, stop and switch to UART serial recovery (`CONFIG_BOOT_SERIAL_UART`, GPIO0/1) per the spec Risk 1 fallback before proceeding.

- [ ] **Step 11: Default-build regression check**

```bash
west build -b rpi_pico -d build_lcd2 -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y
ls -l build_lcd2/zephyr/zephyr.bin
```
Expected: builds clean; size unchanged vs the prerequisite baseline (the default build never reads `sysbuild.conf` / `dfu.conf` and `BOARD` has no `mcuboot`).

- [ ] **Step 12: Commit**

`CMakeLists.txt` is intentionally **not** staged (Step 6 makes no changes to it). Commit only the new build-infra files:

```bash
git add sysbuild.conf sysbuild/mcuboot.conf sysbuild/mcuboot.overlay \
        boards/rpi_pico/dfu_singleslot.dtsi dfu.conf
git commit -m "feat(dfu): MCUboot single-slot + USB-CDC serial-recovery sysbuild spike

Stands up the opt-in --sysbuild -b rpi_pico/rp2040/w/mcuboot upgrade build:
single ~1.84 MB app slot (WiFi image can't fit dual-slot in 2 MB), MCUboot
serial recovery over USB-CDC ACM, entered for now via a temporary WAIT_FOR_DFU
window. Host build-verified: bootloader fits the boot partition with 48%
headroom, both images agree on slot0 @0x18000. Hardware flash/mcumgr deferred
to the operator. No app trigger yet (Task 2). Default builds unaffected (the
app overlays/conf are passed namespaced; CMakeLists is unchanged).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Software trigger — boot-mode retention + `src/dfu/dfu.c` + shell `dfu`

Replace the temporary `WAIT_FOR_DFU` window with the real software trigger: the app writes a retained-RAM boot-mode byte and warm-reboots; MCUboot reads it and stays in serial recovery. Proves Risk 2 (retention survives the RP2040 warm reset).

**Files:**
- Create: `src/dfu/dfu.h`, `src/dfu/dfu.c`
- Modify: `sysbuild/mcuboot.conf`, `dfu.conf`, `CMakeLists.txt`

- [ ] **Step 1: Switch MCUboot to the retention boot-mode (`sysbuild/mcuboot.conf`)**

Delete the temporary `WAIT_FOR_DFU` block (the comment + the two `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU*` lines at the end of the file). **Keep** `CONFIG_BOOT_SERIAL_ENTRANCE_GPIO=n`. Append the retention boot-mode block:

```ini
# Stay in serial recovery when the application set the retention boot-mode flag
# (src/dfu/dfu.c) and rebooted — no button/window for the normal case. The
# retained byte lives in the DT region from boards/rpi_pico/dfu_singleslot.dtsi
# (shared with the app image, so both reference the same RAM address).
CONFIG_RETAINED_MEM=y
CONFIG_RETAINED_MEM_ZEPHYR_RAM=y
CONFIG_RETENTION=y
CONFIG_RETENTION_BOOT_MODE=y
CONFIG_BOOT_SERIAL_BOOT_MODE=y
```

- [ ] **Step 2: Add retention + reboot to the app config (`dfu.conf`)**

Append to `dfu.conf`:

```ini
# Retained-RAM boot mode: dfu_enter_recovery() writes BOOT_MODE_TYPE_BOOTLOADER
# to the retained byte (DT region from boards/rpi_pico/dfu_singleslot.dtsi, the
# same address the bootloader reads), then warm-reboots. RP2040 .noinit RAM is
# not zeroed by a warm sys_reboot(), so the flag survives into MCUboot.
CONFIG_RETAINED_MEM=y
CONFIG_RETAINED_MEM_ZEPHYR_RAM=y
CONFIG_RETENTION=y
CONFIG_RETENTION_BOOT_MODE=y

# sys_reboot() drops into the bootloader. (CONFIG_REBOOT is already on in
# prj.conf; restate here so the upgrade build is self-describing.)
CONFIG_REBOOT=y
```

- [ ] **Step 3: DFU trigger header (`src/dfu/dfu.h`)**

Create `src/dfu/dfu.h`:

```c
/*
 * USB-DFU upgrade trigger. The single entry point every trigger source
 * (shell `dfu`, UDP `DFU`, the on-device button hold) calls to reboot the
 * device into MCUboot serial recovery. Compiled only into the */mcuboot
 * upgrade build (gated in CMakeLists by BOARD MATCHES "mcuboot").
 *
 * Design: docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md
 */
#ifndef DFU_DFU_H_
#define DFU_DFU_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Request a reboot into MCUboot USB serial recovery. Non-blocking: schedules a
 * short (200 ms) delayed work that sets the retention boot-mode flag and warm-
 * reboots, so the caller's reply (UDP datagram / shell line / msgbox repaint)
 * is flushed first. Safe to call from any thread/work context.
 */
void dfu_enter_recovery(void);

#ifdef __cplusplus
}
#endif

#endif /* DFU_DFU_H_ */
```

- [ ] **Step 4: DFU trigger implementation + shell command (`src/dfu/dfu.c`)**

Create `src/dfu/dfu.c`:

```c
#include "dfu.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(dfu, LOG_LEVEL_INF);

/* Deferred so the trigger source can flush its reply before the reboot. */
static void dfu_reboot_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

	if (rc != 0) {
		LOG_ERR("bootmode_set failed (%d); rebooting anyway", rc);
	}
	LOG_INF("rebooting into MCUboot USB serial recovery");
	sys_reboot(SYS_REBOOT_WARM);
}
static K_WORK_DELAYABLE_DEFINE(dfu_reboot_work, dfu_reboot_fn);

void dfu_enter_recovery(void)
{
	LOG_INF("DFU recovery requested; rebooting in 200 ms");
	(void)k_work_schedule(&dfu_reboot_work, K_MSEC(200));
}

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Entering MCUboot USB serial recovery (rebooting)...");
	dfu_enter_recovery();
	return 0;
}
SHELL_CMD_REGISTER(dfu, NULL,
		   "Reboot into MCUboot USB serial recovery for firmware upgrade",
		   cmd_dfu);
#endif /* CONFIG_SHELL */
```

- [ ] **Step 5: Compile `src/dfu/dfu.c` into the upgrade build (`CMakeLists.txt`)**

Add a sources block in the **post-`find_package`** body of `CMakeLists.txt` (e.g. just after the `if(CONFIG_DISPLAY_SPI_PIO_DMA)` block, ~line 102). Gate on `CONFIG_BOOTLOADER_MCUBOOT` — a real Kconfig symbol set by `dfu.conf`, available after `find_package` — **not** on `BOARD MATCHES "mcuboot"` (the sysbuild child does not see `BOARD` reliably; see Task 1 Step 6):

```cmake
# Thin app-side trigger module for the USB-DFU upgrade build. Only compiled when
# MCUboot is the bootloader (it references bootmode_set / sys_reboot). The shell
# `dfu` command inside is further gated on CONFIG_SHELL (WiFi/AP builds).
if(CONFIG_BOOTLOADER_MCUBOOT)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/dfu/dfu.c)
  target_include_directories(app PRIVATE ${PROJECT_SOURCE_DIR}/src/dfu)
endif()
```

- [ ] **Step 6: Rebuild**

```bash
cd ~/Developer/zpsu_mon
# (conda zephyr44 env + toolchain vars sourced as in Task 1 Step 7)
west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_wifi -p always -- \
    -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
    -Dzpsu_mon_EXTRA_CONF_FILE="wifi.conf;dfu.conf" \
    -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi"
```
Expected: both images build. MCUboot no longer waits at boot (the `WAIT_FOR_DFU` lines are gone); it boots the app immediately unless the retained flag is set.

- [ ] **Step 7: Flash + verify the software trigger on hardware (Risk 2)**

Flash (`west flash -d build_dfu_wifi`, or BOOTSEL per Task 1 Step 9). Then over the app's USB-CDC shell:
```
uart:~$ dfu
Entering MCUboot USB serial recovery (rebooting)...
```
The device reboots. Within a couple of seconds, the MCUboot recovery CDC port appears; confirm it stayed in recovery (did NOT boot the app):
```bash
PORT=$(ls /dev/tty.usbmodem* | head -1)
mcumgr --conntype serial --connstring "dev=$PORT,baud=115200" image list
```
Expected: `image list` succeeds → the warm reset preserved the retained flag and MCUboot honoured it (**Risk 2 closed**). Upload a rebuilt image and `reset` (as in Task 1 Step 10); confirm the app boots the new image AND that a normal power-cycle now boots straight to the app (flag cleared after one use).

> **If the flag does NOT survive** (MCUboot boots the app instead of staying in recovery): the RP2040 warm reset cleared the retained byte. Mitigation per spec Risk 2 — add `CONFIG_BOOT_SERIAL_ENTRANCE_GPIO` to `sysbuild/mcuboot.conf` (hold button A at reset) as the entry, and document that instead. Record the finding either way.

- [ ] **Step 8: Default-build regression check**

```bash
west build -b rpi_pico -d build_lcd2 -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: clean; `src/dfu/dfu.c` is not compiled (no `mcuboot` in `BOARD`).

- [ ] **Step 9: Commit**

```bash
git add sysbuild/mcuboot.conf dfu.conf src/dfu/dfu.h src/dfu/dfu.c CMakeLists.txt
git commit -m "feat(dfu): software trigger via retention boot-mode + shell \`dfu\`

dfu_enter_recovery() writes the retained boot-mode byte and warm-reboots;
MCUboot (BOOT_SERIAL_BOOT_MODE) stays in serial recovery. Replaces the Task 1
WAIT_FOR_DFU spike entry. Shell \`dfu\` command (CONFIG_SHELL builds) is the
first trigger. Verified on hardware: the retained flag survives the RP2040
warm reset and MCUboot honours it.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Host-testable `dfu_cmd` matcher + UDP `DFU` verb (TDD)

Add the second trigger — a `DFU` command on the AP build's UDP server — behind a Zephyr-free, host-tested matcher (the `cred_parse` pattern from the spec's testing section).

**Files:**
- Create: `src/dfu/dfu_cmd.h`, `src/dfu/dfu_cmd.c`, `tests/dfu_cmd/test_dfu_cmd.c`
- Modify: `src/net/psu_udp.c`, `CMakeLists.txt`

- [ ] **Step 1: Write the failing host test (`tests/dfu_cmd/test_dfu_cmd.c`)**

Create `tests/dfu_cmd/test_dfu_cmd.c`:

```c
/* Host test (plain clang, no Zephyr): dfu_cmd pure matcher logic. */
#include <assert.h>
#include <stdio.h>
#include "dfu_cmd.h"

static void test_matches(void)
{
	assert(dfu_cmd_is_trigger("DFU") == 1);
	assert(dfu_cmd_is_trigger("dfu") == 1);
	assert(dfu_cmd_is_trigger("Dfu") == 1);
	assert(dfu_cmd_is_trigger("  DFU  ") == 1);     /* surrounding spaces */
	assert(dfu_cmd_is_trigger("\tDFU\r\n") == 1);   /* tab + CRLF */
}

static void test_rejects(void)
{
	assert(dfu_cmd_is_trigger("DF") == 0);          /* too short */
	assert(dfu_cmd_is_trigger("DFUX") == 0);        /* trailing junk */
	assert(dfu_cmd_is_trigger("DFU ON") == 0);      /* extra token */
	assert(dfu_cmd_is_trigger("XDFU") == 0);        /* leading junk */
	assert(dfu_cmd_is_trigger("") == 0);            /* empty */
	assert(dfu_cmd_is_trigger(0) == 0);             /* NULL */
}

int main(void)
{
	test_matches();
	test_rejects();
	printf("dfu_cmd: all tests passed\n");
	return 0;
}
```

- [ ] **Step 2: Run it — verify it fails to compile (no module yet)**

```bash
cd ~/Developer/zpsu_mon
clang -std=c11 -Wall -I src/dfu tests/dfu_cmd/test_dfu_cmd.c src/dfu/dfu_cmd.c -o /tmp/tdfu && /tmp/tdfu
```
Expected: FAIL — `src/dfu/dfu_cmd.c` / `dfu_cmd.h` don't exist (compile error).

- [ ] **Step 3: Write the matcher header (`src/dfu/dfu_cmd.h`)**

Create `src/dfu/dfu_cmd.h`:

```c
/*
 * Zephyr-free matcher for the UDP `DFU` trigger verb (host-testable, the
 * cred_parse pattern). Kept separate from src/dfu/dfu.c (which is on-target
 * only) so the parse logic can be unit-tested with plain clang.
 */
#ifndef DFU_DFU_CMD_H_
#define DFU_DFU_CMD_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return 1 if `line` is exactly the DFU trigger verb "DFU" (case-insensitive,
 * surrounding spaces/tabs/CR/LF ignored), else 0. NULL returns 0.
 */
int dfu_cmd_is_trigger(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* DFU_DFU_CMD_H_ */
```

- [ ] **Step 4: Implement the matcher (`src/dfu/dfu_cmd.c`)**

Create `src/dfu/dfu_cmd.c`:

```c
#include "dfu_cmd.h"

#include <ctype.h>

int dfu_cmd_is_trigger(const char *line)
{
	static const char verb[] = "dfu";

	if (line == 0) {
		return 0;
	}
	while (*line == ' ' || *line == '\t') {     /* skip leading whitespace */
		line++;
	}
	for (int i = 0; i < 3; i++) {               /* case-insensitive "dfu" */
		if (tolower((unsigned char)line[i]) != verb[i]) {
			return 0;
		}
	}
	const char *p = line + 3;                   /* only trailing ws may follow */
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	return (*p == '\0') ? 1 : 0;
}
```

(Reading `line[2]` when the string is shorter is safe: the comparison hits the `'\0'` terminator and returns 0 before any out-of-bounds index.)

- [ ] **Step 5: Run the test — verify it passes**

```bash
clang -std=c11 -Wall -I src/dfu tests/dfu_cmd/test_dfu_cmd.c src/dfu/dfu_cmd.c -o /tmp/tdfu && /tmp/tdfu
```
Expected: `dfu_cmd: all tests passed`.

- [ ] **Step 6: Wire the `DFU` verb into the UDP dispatcher (`src/net/psu_udp.c`)**

In `src/net/psu_udp.c`, add the includes near the existing project includes (after line 31, `#include "ap_config.h"`):

```c
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include "dfu.h"
#include "dfu_cmd.h"
#endif
```

Then in `handle_cmd()`, immediately before the final `return snprintf(out, outsz, "ERR bad command");` (line 119), add the DFU branch. It builds the reply first; `dfu_enter_recovery()` is deferred (200 ms), so the datagram is sent by the thread before the reboot:

```c
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	if (dfu_cmd_is_trigger(line)) {
		/* Reply BEFORE the deferred reboot drops USB; the upgrade then runs
		 * inside MCUboot serial recovery (see src/dfu/dfu.c). */
		int rn = snprintf(out, outsz, "OK DFU (rebooting into recovery)");

		dfu_enter_recovery();
		return rn;
	}
#endif
```

Also extend the `HELP` reply (line 99-100) so the verb is discoverable on upgrade builds — append `|DFU` to the help string. (Leave it unconditional; it documents the verb even where the branch is compiled out.)

- [ ] **Step 7: Compile `dfu_cmd.c` into the AP build (`CMakeLists.txt`)**

In the existing `if(CONFIG_APP_WIFI_AP)` block (lines 115-125), add `src/dfu/dfu_cmd.c` to its `target_sources` and `src/dfu` to its `target_include_directories`, so `psu_udp.c` resolves `dfu_cmd.h` (and `dfu.h` when the `*/mcuboot` block from Task 2 also adds `dfu.c`):

```cmake
if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
    ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c
    ${PROJECT_SOURCE_DIR}/src/net/persist.c
    ${PROJECT_SOURCE_DIR}/src/net/ap_config.c
    ${PROJECT_SOURCE_DIR}/src/net/cred_parse.c
    ${PROJECT_SOURCE_DIR}/src/dfu/dfu_cmd.c)
  target_include_directories(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net
    ${PROJECT_SOURCE_DIR}/src/psu
    ${PROJECT_SOURCE_DIR}/src/dfu)
endif()
```

- [ ] **Step 8: Build the AP upgrade image + verify on hardware**

```bash
cd ~/Developer/zpsu_mon
# (conda zephyr44 env + toolchain vars sourced as in Task 1 Step 7)
west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_ap -p always -- \
    -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
    -Dzpsu_mon_EXTRA_CONF_FILE="ap.conf;dfu.conf" \
    -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi"
west flash -d build_dfu_ap
```
Join the `zpsu-<MAC4>` AP, then:
```bash
printf 'DFU\n' | nc -u 192.168.4.1 5000          # -> OK DFU (rebooting into recovery)
```
Expected: the reply returns, then the device reboots into recovery (confirm with `mcumgr ... image list` on the new CDC port). `printf 'HELP\n' | nc -u 192.168.4.1 5000` lists `...|DFU`.

- [ ] **Step 9: Default-build + AP-non-upgrade regression**

```bash
west build -b rpi_pico -d build_lcd2 -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y
west build -b rpi_pico/rp2040/w -d build_ap -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```
Expected: both clean. The plain AP build (no `mcuboot`, no `CONFIG_BOOTLOADER_MCUBOOT`) compiles `dfu_cmd.c` but the `#if`-guarded DFU branch is out, so `DFU` returns `ERR bad command` there — correct (no upgrade path without the bootloader).

- [ ] **Step 10: Commit**

```bash
git add src/dfu/dfu_cmd.h src/dfu/dfu_cmd.c tests/dfu_cmd/test_dfu_cmd.c src/net/psu_udp.c CMakeLists.txt
git commit -m "feat(dfu): UDP \`DFU\` trigger behind a host-tested matcher

Zephyr-free dfu_cmd_is_trigger() (clang unit test, cred_parse style) drives a
new DFU verb in the AP UDP dispatcher: replies OK then dfu_enter_recovery().
Guarded by CONFIG_BOOTLOADER_MCUBOOT so the plain AP build is unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: On-device button trigger (the non-WiFi builds' only path)

The non-WiFi builds (lcd1-4) have no shell and no UDP — their software trigger must be the physical buttons. Add a deliberate two-button hold (A + Y for 2 s) that pops an `lv_msgbox` and calls `dfu_enter_recovery()`. Uses the `input` subsystem (already `CONFIG_INPUT=y`) and the Pico Display Pack button codes (`INPUT_KEY_1` = A on gpio12, `INPUT_KEY_DOWN` = Y on gpio15).

**Files:**
- Create: `src/dfu/dfu_buttons.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Button-hold listener (`src/dfu/dfu_buttons.c`)**

Create `src/dfu/dfu_buttons.c`:

```c
/*
 * On-device USB-DFU trigger for builds with no shell/UDP (the non-WiFi watch
 * builds). Hold buttons A + Y together for 2 s -> a confirm msgbox -> on the
 * second A+Y hold (or msgbox confirm) -> dfu_enter_recovery(). The deliberate
 * two-button long hold is the guard against accidental entry (spec 5.2).
 *
 * Compiled only into the */mcuboot upgrade build (CMakeLists gate).
 */
#include "dfu.h"

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_kbd_matrix.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(dfu_buttons, LOG_LEVEL_INF);

#define DFU_HOLD_TIME K_SECONDS(2)

static bool a_down;   /* INPUT_KEY_1  (button A) */
static bool y_down;   /* INPUT_KEY_DOWN (button Y) */

static void dfu_confirm_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(dfu_hold_work, dfu_confirm_fn);

/* Runs on the system workqueue after a sustained A+Y hold. LVGL calls must be
 * serialized with the rendering thread; the project already drives LVGL from
 * work items, so this matches the existing pattern. */
static void dfu_confirm_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!(a_down && y_down)) {
		return;   /* released during the hold */
	}

	static const char *btns[] = { "Upgrade", "Cancel", "" };
	lv_obj_t *mbox = lv_msgbox_create(NULL, "Firmware Upgrade",
					  "Reboot into USB recovery?", btns, false);
	lv_obj_center(mbox);
	lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
		lv_obj_t *mb = lv_event_get_current_target(e);
		if (lv_msgbox_get_active_btn(mb) == 0) {   /* "Upgrade" */
			dfu_enter_recovery();
		}
		lv_msgbox_close(mb);
	}, LV_EVENT_VALUE_CHANGED, NULL);

	LOG_INF("A+Y held: DFU confirm shown");
}

static void dfu_keys_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == INPUT_KEY_1) {
		a_down = evt->value;
	} else if (evt->code == INPUT_KEY_DOWN) {
		y_down = evt->value;
	} else {
		return;
	}

	if (a_down && y_down) {
		(void)k_work_schedule(&dfu_hold_work, DFU_HOLD_TIME);
	} else {
		(void)k_work_cancel_delayable(&dfu_hold_work);
	}
}
INPUT_CALLBACK_DEFINE(NULL, dfu_keys_cb, NULL);
```

> **Note (C++ lambda):** the inline `lv_event_cb` lambda compiles under this project's C++ toolchain only if `dfu_buttons.c` is built as C++. Build it as `dfu_buttons.cpp` (rename) **or** hoist the handler to a free function `static void dfu_msgbox_cb(lv_event_t *e)` and pass it by name — pick the free-function form to keep the file C. The free function body is identical to the lambda body above.

- [ ] **Step 2: Compile it into the upgrade build (`CMakeLists.txt`)**

In the `if(CONFIG_BOOTLOADER_MCUBOOT)` sources block from Task 2 Step 5, add `dfu_buttons.c`:

```cmake
if(CONFIG_BOOTLOADER_MCUBOOT)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/dfu/dfu.c
    ${PROJECT_SOURCE_DIR}/src/dfu/dfu_buttons.c)
  target_include_directories(app PRIVATE ${PROJECT_SOURCE_DIR}/src/dfu)
endif()
```

- [ ] **Step 3: Build the non-WiFi upgrade image**

```bash
cd ~/Developer/zpsu_mon
# (conda zephyr44 env + toolchain vars sourced as in Task 1 Step 7)
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build_dfu_lcd2 -p always -- \
    -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
    -Dzpsu_mon_EXTRA_CONF_FILE=dfu.conf \
    -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi"
```
Expected: builds (note board `rpi_pico/rp2040/mcuboot`, the non-W variant; `dfu.conf` only — no shell/UDP). Fits flash + RAM.

- [ ] **Step 4: Flash + verify on hardware**

`west flash -d build_dfu_lcd2` (or BOOTSEL). On the watch screen, hold **A + Y** together for 2 s.
Expected: the "Firmware Upgrade — Reboot into USB recovery?" msgbox appears; pressing **Upgrade** reboots into recovery (confirm with `mcumgr ... image list`). A brief or single-button press does nothing.

- [ ] **Step 5: Default-build regression check**

```bash
west build -b rpi_pico -d build_lcd2 -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: clean; `dfu_buttons` not compiled (no `mcuboot`).

- [ ] **Step 6: Commit**

```bash
git add src/dfu/dfu_buttons.c CMakeLists.txt   # or dfu_buttons.cpp if you renamed
git commit -m "feat(dfu): on-device A+Y button-hold trigger for non-WiFi builds

Two-button 2 s hold -> confirm msgbox -> dfu_enter_recovery(), via the input
subsystem (Pico Display Pack buttons A/Y). Gives the shell-less, UDP-less watch
builds a software upgrade entry. Compiled only for the */mcuboot variant.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: CI build + docs (+ RP2350 follow-on)

Publish the upgrade build from CI and document the upgrade flow. The host `dfu_cmd` test also runs in CI as a fast gate.

**Files:**
- Modify: `.github/workflows/firmware.yml`, `README.md`

- [ ] **Step 1: Build the upgrade variant in CI (`.github/workflows/firmware.yml`)**

After the existing `west build ... build_ap ...` line (line 133), add the sysbuild upgrade build and a merged-image artifact. Keep it a build-only gate (no release asset yet — first install is manual):

```yaml
          # MCUboot USB-DFU upgrade build (opt-in sysbuild + */mcuboot variant).
          # Single-application-slot serial recovery over USB-CDC; proves the
          # upgrade image + bootloader still build. App-image overlays/conf are
          # namespaced (-Dzpsu_mon_*) because a sysbuild child does not see
          # BOARD/EXTRA_CONF_FILE before find_package. No secrets baked in.
          west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_wifi -- \
            -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
            -Dzpsu_mon_EXTRA_CONF_FILE="wifi.conf;dfu.conf" \
            -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi" \
            -Dzpsu_mon_EXTRA_CFLAGS="-DOVP_SCALE_FACTOR=${{ github.event.inputs.ovp_scale_factor }}"
```

- [ ] **Step 2: Run the host `dfu_cmd` test in CI**

Before the `west build` lines (after the toolchain export, ~line 125), add a fast host-test gate:

```yaml
          # Host unit test (no Zephyr): the DFU UDP command matcher.
          clang -std=c11 -Wall -I src/dfu tests/dfu_cmd/test_dfu_cmd.c \
            src/dfu/dfu_cmd.c -o /tmp/tdfu && /tmp/tdfu
```
(If `clang` is absent on the runner, use `cc`/`gcc` — the test is portable C11.)

- [ ] **Step 3: Build-matrix line + upgrade how-to (`README.md`)**

Per the project's minimal-README convention (one build-matrix line per target; detail stays in the spec), add to the compile section (after the AP build, ~line 67) a single block:

````markdown
    Pico W RP2040 USB-DFU upgrade build (MCUboot serial recovery; opt-in --sysbuild)
    west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_wifi -- \
        -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
        -Dzpsu_mon_EXTRA_CONF_FILE="wifi.conf;dfu.conf" \
        -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi"
    First install: west flash -d build_dfu_wifi (debug probe; flashes both images) or
    BOOTSEL-drag mcuboot.uf2 + a picotool-converted signed-app uf2. Upgrades after
    that are USB-only, no button:
      trigger  -> shell `dfu`  |  UDP `printf 'DFU\n' | nc -u 192.168.4.1 5000`  |  hold A+Y 2 s
      upload   -> mcumgr --conntype serial --connstring "dev=<port>,baud=115200" \
                    image upload build_dfu_wifi/zpsu_mon/zephyr/zephyr.signed.bin
      boot new -> mcumgr --conntype serial --connstring "dev=<port>,baud=115200" reset
    Design + partition map + RP2350 notes: docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md
````

- [ ] **Step 4: Record the RP2350 follow-on in the spec's scope**

The on-metal tasks proved RP2040. RP2350 (`rpi_pico2/rp2350a/m33/mcuboot`, 4 MB) reuses the identical mechanism but needs its **own** overlay: a 4 MB single-slot map + `#include <raspberrypi/rp2350-boot-mode-retention.dtsi>`, e.g. `boards/rpi_pico2/dfu_singleslot.dtsi`, plus a CMake arm `if(BOARD MATCHES "mcuboot" AND BOARD MATCHES "rp2350")`. Add a one-line "RP2350 follow-on" note under §9 (Out of scope → "future option") of `docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md` pointing here, so the next builder knows the only delta is the sibling overlay. (Do not implement RP2350 here — it has no on-metal coverage in this plan.)

- [ ] **Step 5: Verify CI locally (dry sanity) + commit**

```bash
cd ~/Developer/zpsu_mon
clang -std=c11 -Wall -I src/dfu tests/dfu_cmd/test_dfu_cmd.c src/dfu/dfu_cmd.c -o /tmp/tdfu && /tmp/tdfu
# (full CI runs the sysbuild build on the runner)
git add .github/workflows/firmware.yml README.md docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md
git commit -m "ci+docs: build the MCUboot USB-DFU upgrade variant; run dfu_cmd host test

CI now builds rpi_pico/rp2040/w/mcuboot (sysbuild) and runs the host dfu_cmd
matcher test. README gains one build-matrix block with the trigger/upload/boot
flow; the spec records the RP2350 follow-on (sibling 4 MB overlay only).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review (against the spec)

**Spec coverage:**
- §2 single-application-slot MCUboot → Task 1 (`SB_CONFIG_MCUBOOT_MODE_SINGLE_APP`, `CONFIG_SINGLE_APPLICATION_SLOT`). ✓
- §2 serial recovery over USB-CDC ACM → Task 1 (`BOOT_SERIAL_CDC_ACM` + CDC overlay). ✓
- §2/§5.1 software trigger via retention boot-mode + reboot → Task 2 (`src/dfu/dfu.c`, `bootmode_set` + warm `sys_reboot`). ✓
- §5.2 three entry points: shell `dfu` (Task 2), UDP `DFU` (Task 3), on-screen UI (Task 4 — realized as the A+Y button hold + msgbox, the only viable on-device input on the button-only Display Pack). ✓
- §4 custom single-slot 2 MB partition map → Task 1 (`dfu_singleslot.dtsi`). ✓
- §2 opt-in `*/mcuboot` sysbuild target; default builds unchanged → every task has a `build_lcd2` regression check; all gates key off `--sysbuild`/`BOARD MATCHES mcuboot`. ✓
- §6 first install (BOOTSEL/probe) + USB-only upgrades → Task 1 Step 9-10, README Task 5. ✓
- §8 host-testable parser test for UDP `DFU` → Task 3 (`tests/dfu_cmd`, clang). ✓
- §7 Risk 1 (footprint/CDC) → Task 1 is the spike with explicit fail-paths; Risk 2 (retention survival) → Task 2 Step 7 with GPIO fallback; Risk 3 (signing key) → `sysbuild.conf` note + README; Risk 4 (CDC coexistence) → distinct product strings, sequential operation. ✓
- §9 out of scope (WiFi OTA, A/B rollback, RP2350 dual-slot) → not built; RP2350 single-slot recorded as a follow-on in Task 5 Step 4. ✓

**Placeholder scan:** every code/config/command step contains the actual content. The one deliberate decision point is the C++-lambda-vs-free-function note in Task 4 Step 1 (both forms given).

**Type/name consistency:** `dfu_enter_recovery()` (dfu.h) called from dfu.c shell cmd, psu_udp.c, dfu_buttons.c. `dfu_cmd_is_trigger()` (dfu_cmd.h) used in test + psu_udp.c. `boot_partition`/`slot0_partition`/`storage_partition` labels consistent across `dfu_singleslot.dtsi` and the board variant's `chosen zephyr,code-partition = &slot0_partition`. Build dirs `build_dfu_wifi` / `build_dfu_ap` / `build_dfu_lcd2` consistent across tasks + CI + README.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-09-usb-dfu-mcuboot-upgrade.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. Note: Tasks 1, 2, 4 have **on-hardware** verification steps that need the physical Pico W + debug probe / BOOTSEL + `mcumgr`; a subagent cannot perform those, so it stops at each hardware gate for you to run the flash/`mcumgr` steps and report results.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints for review.

**Which approach?**
