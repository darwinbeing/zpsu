# Pico W (RP2040) WiFi Connectivity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the Pico W's CYW43439 WiFi up to a working DHCP-assigned network connection on the `rpi_pico/rp2040/w` board, driven interactively from a USB-CDC shell, with zero impact on the existing non-W builds.

**Architecture:** An opt-in Kconfig fragment (`wifi.conf`, passed via `-DEXTRA_CONF_FILE`) enables Zephyr's networking + WiFi + shell on top of the project's existing config. A small source module (`src/net/wifi_net.c`), compiled only when `CONFIG_WIFI` is set, registers `net_mgmt` callbacks to auto-start DHCPv4 on association and log the acquired IP. The board's own devicetree supplies the CYW43439 (PIO0 SPI), so no DT changes are made here. Credentials are entered at runtime via the `wifi connect` shell command — nothing is stored in the repo.

**Tech Stack:** Zephyr v4.2.1, Raspberry Pi Pico W (RP2040 + Infineon CYW43439), `infineon,airoc-wifi` driver, Zephyr networking (IPv4/DHCPv4), Zephyr shell over USB CDC ACM, SEGGER RTT for logs.

---

## Important notes for the implementer

- **This is on-target embedded firmware.** There is no host unit-test harness. "Verification" for each task means: (a) the build succeeds and **fits RP2040 RAM**, and (b) **on-hardware** behaviour observed through the USB-CDC shell and the RTT log. Do not invent pytest-style tests.
- **Branch:** work on `feat/pico-w-wifi` (already created; the design spec is committed there).
- **The console/shell-uart are already wired to `usb_cdc`** in `boards/rpi_pico/pico_display_pack2.overlay` (and the other pack overlays). We only enable the shell — no DT edits.
- **RAM is the primary risk** (RP2040 has 264 KB; LVGL already uses ~95 KB pool + 33 KB heap + double VDB). Task 1 is deliberately the build-and-fit gate, *before* any C code. The trim order if it overflows is baked into `wifi.conf` comments and Task 1 Step 4.
- **Spec:** `docs/superpowers/specs/2026-06-07-pico-w-wifi-design.md`.

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `wifi.conf` (repo root) | Opt-in Kconfig fragment: networking + WiFi + shell, trimmed for RP2040 | 1 |
| `src/net/wifi_net.c` | `net_mgmt` glue: auto-DHCP on associate, log IP on bind. Gated by `CONFIG_WIFI` | 2 |
| `CMakeLists.txt` | Add `wifi_net.c` to the build under `if(CONFIG_WIFI)` | 2 |
| `README.md` | Document the Pico W WiFi build (blobs + build line + shell usage) | 3 |

> Note: no `wifi_net.h` is created. The module self-registers via `SYS_INIT` and exposes no API, so a header would be dead weight (YAGNI). This resolves the spec's open question (SYS_INIT vs app hook) in favour of `SYS_INIT`.

---

## Prerequisites (one-time, not a commit)

- [ ] **Fetch the CYW43439 firmware blobs** (required to build for the W board):

Run:
```bash
cd ~/Developer/zephyrproject
west blobs fetch hal_infineon
```
Expected: downloads complete; afterwards
`find ~/Developer/zephyrproject -ipath '*hal_infineon*' -iname '*.bin' | grep -i 43439`
prints at least one firmware/CLM blob. If it prints nothing, the WiFi build will fail at link/runtime — do not proceed until blobs are present.

- [ ] **Confirm the build environment** uses the Anaconda Python and the Zephyr tree under `~/Developer/zephyrproject` (per project memory), and that `west build` works for an existing target, e.g.:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico -d build_lcd2 -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: builds clean (this is the known-good baseline; if this fails, fix the toolchain before starting).

---

## Task 1: Opt-in `wifi.conf` fragment — stand up the stack and the shell

**Files:**
- Modify: `boards/rpi_pico/pico_display_pack2.overlay` (W-board DT compat — see Step 0)
- Create: `wifi.conf`

This task brings up the heavy/risky part — the WiFi driver, the networking stack, the shell, and the RAM fit — **without any project C code**, so DHCP is exercised manually from the shell. De-risks before Task 2.

- [ ] **Step 0: Make the pack2 overlay build on the Pico W**

The `rpi_pico/rp2040/w` board's `.dts` does **not** include Zephyr's
`rpi_pico-led.dtsi` (on the Pico W, GP25 belongs to the CYW43439, not an
onboard LED). That file is the only place the `pwm_led0` label is defined; on
the plain Pico it happens to alias the same `/pwm_leds/pwm_led_0` node that
this project labels `display_blk` (the display backlight). So
`&pwm_led0` in the pack overlay resolves on the non-W board by coincidence but
is an **undefined label on the W board** → `parse error: undefined node label
'pwm_led0'`.

Fix: reference the backlight by the project's own portable label. In
`boards/rpi_pico/pico_display_pack2.overlay`, replace the `&pwm_led0` block
(near line 59) with:

```dts
/* Display backlight PWM. Use this project's own label (display_blk) rather
 * than &pwm_led0: the latter is defined only by the non-W board's
 * rpi_pico-led.dtsi, which rpi_pico/rp2040/w does not include. Both labels
 * name the same /pwm_leds/pwm_led_0 node, so this is a no-op on the plain
 * Pico and lets the overlay build on the Pico W. */
&display_blk {
	status = "okay";
};
```

Leave the `&pwm_led1` block unchanged — `pwm_led1` is defined by this
project's `pico_display_pack_leds.dtsi`, so it exists on both boards.
(Scope note: only the pack2 overlay is fixed, since the WiFi build uses
`-DCONFIG_PICO_DISPLAY_PACK2=y`. The identical latent issue in `pico_display_pack.overlay` / the `rpi_pico2` overlays is left untouched.)

- [ ] **Step 0b: Make the heartbeat-LED module build on the Pico W**

Same root cause, in C: `src/led/led.cpp` uses `DT_ALIAS(led0)` (the GP25
onboard LED), an alias defined only by `rpi_pico-led.dtsi` — absent on the W
board, so `GPIO_DT_SPEC_GET(LED0_NODE, gpios)` fails to compile. Fix with the
null-safe `_OR` variant the codebase already uses for `display_blk`
(`display_control.cpp:17`). In `src/led/led.cpp`, change:

```cpp
const struct gpio_dt_spec led_spec = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
```
to:
```cpp
/* GET_OR (not GET) so this builds on boards without an onboard led0 — e.g. the
 * Pico W, where GP25 belongs to the CYW43439 and rpi_pico-led.dtsi is omitted.
 * The empty fallback spec makes gpio_is_ready_dt() fail in Start(), so the
 * heartbeat LED is simply a no-op there. Mirrors display_blk in
 * display_control.cpp; byte-identical behaviour on the plain Pico. */
const struct gpio_dt_spec led_spec = GPIO_DT_SPEC_GET_OR(LED0_NODE, gpios, {});
```
The existing `gpio_is_ready_dt(&led_spec)` early-return in `Led::Start()`
already handles the empty spec, so no other change is needed. (Audit of all
`DT_ALIAS`/`DT_NODELABEL` uses in `src/` confirms `led0` is the only remaining
W-board gap; the RGB LEDs, `display_blk`, `i2c0`, and `regulator_3v3_ctrl`
are all defined on the W board or already use `_OR`/`_OR_NULL`.)

- [ ] **Step 1: Create `wifi.conf`**

Create `wifi.conf` at the repo root. Values are based on Zephyr's validated `samples/net/wifi/shell/prj.conf`, trimmed for RP2040 RAM:

```ini
# --- Pico W (RP2040) WiFi connectivity (opt-in fragment) ---
# Build:  west build -b rpi_pico/rp2040/w -d build_wifi -- \
#             -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf
# Requires: west blobs fetch hal_infineon  (one-time, in zephyrproject)
#
# Default rpi_pico / rpi_pico2 builds never include this file, so they are
# completely unaffected.

# Networking core: IPv4 + DHCP only. No TCP/IPv6 to save RP2040 RAM
# (the connectivity foundation needs only DHCP + ICMP).
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_IPV6=n
CONFIG_NET_TCP=n
CONFIG_NET_DHCPV4=y
CONFIG_NET_MAX_CONTEXTS=6

# WiFi: the board defconfig already sets CONFIG_CYW43439=y and the board DT
# enables the AIROC driver; this turns on the WiFi management API.
CONFIG_WIFI=y
CONFIG_WIFI_LOG_LEVEL_ERR=y

# Interactive shell over the existing USB CDC ACM console (already the chosen
# zephyr,shell-uart in the pack overlay). Keep logs on RTT, not in the shell.
CONFIG_SHELL=y
CONFIG_NET_SHELL=y
CONFIG_NET_L2_WIFI_SHELL=y
CONFIG_SHELL_LOG_BACKEND=n

# net_mgmt event delivery (scan results are bursty).
CONFIG_NET_MGMT_EVENT_QUEUE_SIZE=16
CONFIG_NET_MGMT_EVENT_QUEUE_TIMEOUT=5000

# Buffers / stacks — trimmed for RP2040. prj.conf sets MAIN_STACK_SIZE=2048;
# raise it here for the net stack. TRIM ORDER if the image overflows RAM:
#   1) (already done) NET_TCP=n, NET_IPV6=n
#   2) lower NET_PKT_* / NET_BUF_* counts
#   3) lower CONFIG_LV_Z_MEM_POOL_SIZE (add it here; do NOT edit prj.conf)
CONFIG_NET_PKT_RX_COUNT=6
CONFIG_NET_PKT_TX_COUNT=6
CONFIG_NET_BUF_RX_COUNT=12
CONFIG_NET_BUF_TX_COUNT=12
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SHELL_STACK_SIZE=3072
CONFIG_NET_TX_STACK_SIZE=1536
CONFIG_NET_RX_STACK_SIZE=1536
```

- [ ] **Step 2: Build for the Pico W**

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico/rp2040/w -d build_wifi -p auto -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf
```
Expected: configuration shows `Compiling for RPI_PICO`, the build completes, and a memory report prints, e.g. `Memory region   Used Size   Region Size   %age Used` with **RAM under 100%**.

- [ ] **Step 3: Confirm WiFi + networking actually got configured in**

Run:
```bash
grep -E 'CONFIG_(WIFI|WIFI_AIROC|CYW43439|NET_L2_WIFI_SHELL|NET_DHCPV4|SHELL)=y' build_wifi/zephyr/.config
```
Expected: all of `CONFIG_WIFI=y`, `CONFIG_WIFI_AIROC=y`, `CONFIG_CYW43439=y`, `CONFIG_NET_L2_WIFI_SHELL=y`, `CONFIG_NET_DHCPV4=y`, `CONFIG_SHELL=y` are present. (`WIFI_AIROC`/`CYW43439` come from the board; their presence confirms the W board + DT were picked up.)

- [ ] **Step 4: If — and only if — RAM overflowed in Step 2**

If Step 2 reported `region 'RAM' overflowed by N bytes`, apply the trim order from the `wifi.conf` comment, lowest-impact first, then rebuild:
1. Lower `CONFIG_NET_BUF_RX_COUNT`/`CONFIG_NET_BUF_TX_COUNT` to `8`, and `CONFIG_NET_PKT_*` to `4`.
2. If still over, add `CONFIG_LV_Z_MEM_POOL_SIZE=80000` to `wifi.conf` (this overrides the 95000 in `prj.conf` for the WiFi build only). Re-run Step 2 until RAM is under 100%.

Skip this step entirely if Step 2 already fit.

- [ ] **Step 5: Flash and verify the shell + driver on hardware**

Flash (UF2: hold BOOTSEL, copy `build_wifi/zephyr/zephyr.uf2`; or `west flash` with your probe). Open the USB-CDC serial port (e.g. `screen /dev/tty.usbmodem* 115200`, any baud — CDC ignores it). Then in the shell:
```
uart:~$ wifi
uart:~$ wifi scan
uart:~$ wifi connect "<your-ssid>" <your-psk>
uart:~$ net dhcpv4 client start 1
uart:~$ net iface
```
Expected: `wifi` lists subcommands; `wifi scan` lists nearby APs; `wifi connect` reports `Connection requested`/`Connected`; after `net dhcpv4 client start 1`, `net iface` shows an `IPv4 unicast address` that is not `0.0.0.0`. (`1` is the iface index — if `net iface` lists the WiFi iface under a different number, use that.) This proves blobs + driver + stack + RAM all work, independent of any project code.

- [ ] **Step 6: Commit** (two commits — the DT fix is a distinct concern)

```bash
cd ~/Developer/zpsu_mon
git add boards/rpi_pico/pico_display_pack2.overlay
git commit -m "fix(board): reference display_blk so pack2 overlay builds on Pico W

&pwm_led0 is defined only by the non-W rpi_pico-led.dtsi; the
rpi_pico/rp2040/w target omits it (GP25 is the CYW43439). display_blk
labels the same backlight node, so this is a no-op on the plain Pico.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"

git add wifi.conf
git commit -m "feat(net): opt-in wifi.conf fragment for Pico W (CYW43439)

Enables networking + WiFi + shell over USB CDC for rpi_pico/rp2040/w
builds via -DEXTRA_CONF_FILE=wifi.conf. IPv4/DHCP only, no TCP/IPv6,
trimmed buffers for RP2040 RAM. Non-W builds are unaffected.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `wifi_net.c` — auto-DHCP on associate + log the IP

**Files:**
- Create: `src/net/wifi_net.c`
- Modify: `CMakeLists.txt` (add the `if(CONFIG_DISPLAY_SPI_PIO_DMA)` neighbourhood block)

After this task, a bare `wifi connect` is enough: the device starts DHCP itself and prints the IP to the RTT log — no manual `net dhcpv4 client start`.

- [ ] **Step 1: Create `src/net/wifi_net.c`**

```c
/*
 * Pico W WiFi connectivity glue.
 *
 * Registers net_mgmt callbacks to (1) start DHCPv4 automatically once the
 * CYW43439 associates with an AP, and (2) log the acquired IPv4 address when
 * the lease binds. Compiled only when CONFIG_WIFI is set (the wifi.conf opt-in
 * fragment). Credentials are entered at runtime via `wifi connect`.
 *
 * Design: docs/superpowers/specs/2026-06-07-pico-w-wifi-design.md
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(wifi_net, LOG_LEVEL_INF);

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status =
			(const struct wifi_status *)cb->info;

		if (status->status) {
			LOG_WRN("WiFi connect failed (status %d)",
				status->status);
		} else {
			LOG_INF("WiFi associated; starting DHCPv4");
			net_dhcpv4_start(iface);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_INF("WiFi disconnected");
		break;
	default:
		break;
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	char ip[NET_IPV4_ADDR_LEN];
	char gw[NET_IPV4_ADDR_LEN];

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *if_addr =
			&iface->config.ip.ipv4->unicast[i].ipv4;

		if (if_addr->addr_type != NET_ADDR_DHCP) {
			continue;
		}

		net_addr_ntop(AF_INET, &if_addr->address.in_addr,
			      ip, sizeof(ip));
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw,
			      gw, sizeof(gw));
		LOG_INF("WiFi up: %s gw %s", ip, gw);
		break;
	}
}

static int wifi_net_init(void)
{
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	LOG_INF("WiFi glue ready; connect with: wifi connect \"<ssid>\" <psk>");
	return 0;
}

SYS_INIT(wifi_net_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 2: Gate the source in `CMakeLists.txt`**

Add this block immediately after the existing `if(CONFIG_DISPLAY_SPI_PIO_DMA) ... endif()` block (around `CMakeLists.txt:74`), before the trailing `# add_subdirectory(src/ui)` comment:

```cmake
# Pico W WiFi connectivity glue — compiled only for the opt-in WiFi build
# (CONFIG_WIFI comes from wifi.conf via -DEXTRA_CONF_FILE). Keeps default
# rpi_pico / rpi_pico2 builds untouched.
if(CONFIG_WIFI)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c)
endif()
```

- [ ] **Step 3: Rebuild**

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico/rp2040/w -d build_wifi -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf
```
Expected: `wifi_net.c` compiles (visible in the build output / no "unused" warnings since `SYS_INIT` references the function), build completes, RAM still under 100%.

- [ ] **Step 4: Sanity-check the default build is still clean**

Run (proves the gate works — `wifi_net.c` must NOT be compiled here):
```bash
west build -b rpi_pico -d build_lcd2 -p auto -- -DCONFIG_PICO_DISPLAY_PACK2=y
grep -c wifi_net build_lcd2/CMakeFiles/app.dir/* 2>/dev/null || echo "wifi_net not in default build (correct)"
```
Expected: builds clean and `wifi_net` is absent from the default build's object list.

- [ ] **Step 5: Flash and verify auto-DHCP on hardware**

Flash `build_wifi/zephyr/zephyr.uf2`. Open an RTT log viewer (JLinkRTTViewer or your usual SEGGER RTT viewer — logs go to RTT, not the shell). In the USB-CDC shell:
```
uart:~$ wifi connect "<your-ssid>" <your-psk>
```
Expected, in the **RTT log**: `WiFi associated; starting DHCPv4` followed shortly by `WiFi up: <ip> gw <gw>` with a real address. Then confirm reachability from the shell:
```
uart:~$ net ping <gateway-ip>
```
Expected: ICMP replies (round-trip times printed). This is the full success criterion from the spec.

- [ ] **Step 6: Commit**

```bash
cd ~/Developer/zpsu_mon
git add src/net/wifi_net.c CMakeLists.txt
git commit -m "feat(net): auto-DHCP + IP logging glue for Pico W WiFi

On WiFi associate, start DHCPv4; on lease bind, log the acquired IPv4
address and gateway over RTT. SYS_INIT-registered net_mgmt callbacks,
compiled only under CONFIG_WIFI. \`wifi connect\` alone now yields an IP.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Document the Pico W WiFi build in the README

**Files:**
- Modify: `README.md` (append to the build matrix near the existing `Pico RP2040 Display Pack2` block)

- [ ] **Step 1: Read the current build section**

Run:
```bash
sed -n '/### Build on Mac OS/,$p' README.md
```
Confirm the existing `west build -b rpi_pico ...` matrix so the new entry matches its style/indentation.

- [ ] **Step 2: Append the Pico W WiFi entry**

After the existing `Pico RP2040 Display Pack2` build line, add a new subsection (match the surrounding two-space-indent code style):

```markdown

### Raspberry Pi Pico W (RP2040) — WiFi

One-time, fetch the CYW43439 firmware blobs:

    cd ~/zephyrproject
    west blobs fetch hal_infineon

Build the Display Pack2 image with WiFi:

    cd ~/zpsu_mon
    west build -b rpi_pico/rp2040/w -d build_wifi -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf

Connect at runtime over the USB-CDC shell (credentials are not stored on the device):

    uart:~$ wifi scan
    uart:~$ wifi connect "<ssid>" <psk>

On association the device auto-runs DHCPv4 and logs the acquired IP over RTT
(`WiFi up: <ip> gw <gw>`). Verify with `net iface` / `net ping <host>`.

Notes: WiFi uses PIO0, so it cannot be combined with the opt-in PIO+DMA
display driver (`CONFIG_DISPLAY_SPI_PIO_DMA`); the WiFi build uses the default
PL022 display path.
```

- [ ] **Step 3: Commit**

```bash
cd ~/Developer/zpsu_mon
git add README.md
git commit -m "docs: document the Pico W WiFi build and shell usage

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review (completed during planning)

- **Spec coverage:** opt-in fragment + CMake gate → Tasks 1–2; runtime WiFi shell over USB CDC → Task 1 (Step 5) config + Task 2 verify; auto-DHCP + IP log glue → Task 2; board target / no-DT-change / blobs → Prerequisites + Task 1; RAM-fit checkpoint + trim order → Task 1 Steps 2/4; non-W builds untouched → Task 2 Step 4; PIO0 / PL022 constraint → README Task 3 + (already in spec). All spec requirements map to a task.
- **Placeholder scan:** no TBD/TODO; all code blocks are complete and all referenced symbols (`net_dhcpv4_start`, `net_if_get_first_wifi` not needed since the event delivers `iface`, `wifi_status.status`, `NET_EVENT_*`, `SYS_INIT`) are real Zephyr v4.2.1 APIs verified against the tree. Credential placeholders (`<your-ssid>`) are runtime user input, not plan gaps.
- **Type consistency:** handler signatures use `uint64_t mgmt_event` (correct for v4.2.1); `wifi_net.c` exposes no symbols, so there is no cross-task interface to keep in sync; the `CONFIG_WIFI` gate symbol matches between `wifi.conf` (Task 1) and `CMakeLists.txt` (Task 2).
