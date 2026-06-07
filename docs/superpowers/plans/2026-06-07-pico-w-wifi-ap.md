# Pico W (RP2040) WiFi AP mode + UDP PSU control — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Pico W its own WiFi Access Point so a phone/laptop joins it directly and drives the PSU over a small UDP text protocol (`STATUS`/`ON`/`OFF`/`MODE`/`CC`/`FAN`), with zero impact on the default and existing STA builds.

**Architecture:** A second opt-in Kconfig fragment (`ap.conf`, via `-DEXTRA_CONF_FILE`) turns on a new project symbol `CONFIG_APP_WIFI_AP` plus WiFi-AP + DHCPv4-server + sockets. `src/net/wifi_ap.c` (AP-gated) brings up the SoftAP at boot via the AIROC driver's `ap_enable`, assigns the iface a static `192.168.4.1/24`, and starts a DHCP server. `src/net/psu_udp.c` (AP-gated) runs a UDP server thread on `:5000` that dispatches text commands to a new mutex-guarded `psu_cmd_*` façade in `psu_service`, through which the LVGL UI callbacks are also routed (closing a pre-existing controller race). AP-only: the driver rejects STA+AP with `-EBUSY`.

**Tech Stack:** Zephyr v4.2.1, Raspberry Pi Pico W (RP2040 + Infineon CYW43439), `infineon,airoc-wifi` driver (`ap_enable`/`ap_disable`), Zephyr networking (IPv4, DHCPv4 server, BSD sockets/UDP), zbus, USB-CDC shell.

---

## Important notes for the implementer

- **This is on-target embedded firmware.** There is no host unit-test harness for the firmware. "Verification" per task means: (a) the build succeeds and **fits RP2040 RAM**, and (b) **on-hardware** behaviour observed through the USB-CDC shell/log and a phone + `nc`. Do not invent pytest-style firmware tests. The one genuine automated test is a **host-side** Python smoke script (Task 5) that talks UDP to the running device.
- **Branch:** work on `feat/pico-w-wifi-ap` (already created; the design spec is committed there).
- **AP-only is forced by the driver.** `airoc_mgmt_ap_enable` returns `-EBUSY` if a STA connection is up, and brings the AP up on a WHD *secondary interface* exposed on the **same single Zephyr `net_if`** (`net_if_get_first_wifi()`). So there is exactly one WiFi iface; we never run STA glue in this build.
- **`ap.conf` and `wifi.conf` are mutually exclusive** — exactly one `EXTRA_CONF_FILE`. Both set `CONFIG_WIFI=y`, so the CMake gate keys off the dedicated `CONFIG_APP_WIFI_AP` symbol, and the STA source (`wifi_net.c`) is gated to NOT compile when `APP_WIFI_AP` is set.
- **RAM is the primary risk** (RP2040 264 KB; STA build already ~89 %). Task 1 is the build-and-fit gate *before* any project C code. Trim order is baked into `ap.conf` comments.
- **`psu_service.cpp` is in every build.** The `psu_cmd_*` façade (Task 3) is always-compiled and must not change default-build behaviour — verify the default build + on-device UI after Task 3.
- **Spec:** `docs/superpowers/specs/2026-06-07-pico-w-wifi-ap-design.md`.

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `Kconfig` (repo root) | Add `config APP_WIFI_AP` (the AP-build gate symbol) | 1 |
| `ap.conf` (repo root) | Opt-in fragment: WiFi + AP, DHCPv4 **server**, UDP sockets, RAM trims | 1 |
| `src/net/wifi_ap.c` | Boot-time AP enable + static IP + DHCP server. Gated by `APP_WIFI_AP` | 2 |
| `src/net/ap_creds.h.sample` | Template for overriding SSID/PSK/channel (real `ap_creds.h` gitignored) | 2 |
| `.gitignore` | Ignore `src/net/ap_creds.h` | 2 |
| `src/psu/psu_service.h` / `.cpp` | `psu_cmd_*` thread-safe façade + `struct psu_status`; UI routed through it | 3 |
| `src/net/psu_udp.c` | UDP server thread on `:5000`, parses text verbs → `psu_cmd_*`. AP-gated | 4 |
| `CMakeLists.txt` | Gate `wifi_ap.c` + `psu_udp.c` under `APP_WIFI_AP`; exclude `wifi_net.c` there | 2, 4 |
| `psu_ap_smoke.py` (repo root) | Host-side UDP smoke test (style of existing `i2c_scan.py`) | 5 |
| `README.md` | One build-matrix line for the AP build | 5 |

---

## Prerequisites (one-time, not a commit)

- [ ] **CYW43439 blobs present** (already fetched for the STA work; confirm):

Run:
```bash
find ~/Developer/zephyrproject -ipath '*hal_infineon*' -iname '*.bin' | grep -i 43439
```
Expected: at least one firmware/CLM blob path prints. If empty, run `cd ~/Developer/zephyrproject && west blobs fetch hal_infineon` first.

- [ ] **Known-good baselines build** (per project memory: conda Python, `ZEPHYR_BASE=~/Developer/zephyrproject/zephyr`):

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico -d build_lcd2 -p auto -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: builds clean (the default non-W baseline). If it fails, fix the toolchain before starting.

---

## Task 1: `ap.conf` + `APP_WIFI_AP` symbol — bring up the AP on metal (no project C)

**Files:**
- Modify: `Kconfig` (add `config APP_WIFI_AP`)
- Create: `ap.conf`

This task stands up the AP, DHCP-server, and sockets Kconfig and **proves the AIROC driver brings up a joinable AP on this hardware** using only the shell `wifi ap enable` — before any project C code. This is the #1 de-risk.

- [ ] **Step 1: Add the gate symbol to `Kconfig`**

In `Kconfig`, inside the `menu "ZSPSUMon"` block, immediately after the `config DISPLAY_SPI_PIO_DMA ... help ...` entry (before the `endmenu` at line 45), add:

```kconfig
    config APP_WIFI_AP
        bool "Pico W: WiFi AP mode + UDP PSU control"
        default n
        help
          Opt-in (set by ap.conf): run the Pico W as a WiFi Access Point and
          expose a small UDP text protocol on :5000 that drives the PSU. Selects
          the AP glue (src/net/wifi_ap.c) and the UDP server (src/net/psu_udp.c),
          and excludes the STA glue (src/net/wifi_net.c). Mutually exclusive with
          the STA wifi.conf build. Default builds are unaffected.
```

- [ ] **Step 2: Create `ap.conf`**

Create `ap.conf` at the repo root:

```ini
# --- Pico W (RP2040) WiFi AP mode + UDP PSU control (opt-in fragment) ---
# Build:  west build -b rpi_pico/rp2040/w -d build_ap -- \
#             -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
# Requires: west blobs fetch hal_infineon  (one-time, in zephyrproject)
#
# Mutually exclusive with wifi.conf (pick one EXTRA_CONF_FILE). Default
# rpi_pico / rpi_pico2 builds never include this file, so they are unaffected.

# Project gate: selects wifi_ap.c + psu_udp.c, excludes wifi_net.c (STA).
CONFIG_APP_WIFI_AP=y

# Networking core: IPv4 + UDP only. No TCP/IPv6 to save RP2040 RAM.
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_IPV6=n
CONFIG_NET_TCP=n
CONFIG_NET_UDP=y
CONFIG_NET_MAX_CONTEXTS=6

# DHCPv4 *server* (hands joining clients an address). No DHCP client in AP mode.
# Selects NET_SOCKETS + NET_SOCKETS_SERVICE; default pool is 4 leases.
CONFIG_NET_DHCPV4_SERVER=y
CONFIG_NET_SOCKETS=y

# WiFi: board defconfig sets CONFIG_CYW43439=y and enables the AIROC driver;
# this turns on the WiFi management API (which includes ap_enable/ap_disable).
CONFIG_WIFI=y
CONFIG_WIFI_LOG_LEVEL_ERR=y

# Leave CONFIG_WIFI_INIT_PRIORITY at its default (80): airoc chip bring-up must
# run before net-if init (NET_INIT_PRIO=90), which reads the chip MAC into the
# link address (we derive the AP SSID suffix from it). (Same lesson as wifi.conf.)

# Interactive shell over the existing USB CDC ACM console. This board has NO
# J-Link, so route logs through the shell over USB-CDC (drop SEGGER RTT) — the
# only console you can read here. NET_L2_WIFI_SHELL gives `wifi ap enable` for
# the Task 1 de-risk.
CONFIG_SHELL=y
CONFIG_NET_SHELL=y
CONFIG_NET_L2_WIFI_SHELL=y
CONFIG_SHELL_LOG_BACKEND=y
CONFIG_USE_SEGGER_RTT=n
CONFIG_LOG_BACKEND_RTT=n

# net_mgmt event delivery.
CONFIG_NET_MGMT_EVENT_QUEUE_SIZE=16
CONFIG_NET_MGMT_EVENT_QUEUE_TIMEOUT=5000

# Buffers / stacks — trimmed for RP2040 (same base as wifi.conf). TRIM ORDER if
# the image overflows RAM:
#   1) (already done) NET_TCP=n, NET_IPV6=n
#   2) lower NET_PKT_* / NET_BUF_* counts
#   3) lower CONFIG_LV_Z_MEM_POOL_SIZE (already 80000 below; drop further)
CONFIG_NET_PKT_RX_COUNT=6
CONFIG_NET_PKT_TX_COUNT=6
CONFIG_NET_BUF_RX_COUNT=12
CONFIG_NET_BUF_TX_COUNT=12
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SHELL_STACK_SIZE=3072
CONFIG_NET_TX_STACK_SIZE=1536
CONFIG_NET_RX_STACK_SIZE=1536

# Random source for the net stack (no RP2040 entropy driver in v4.2.1).
CONFIG_TEST_RANDOM_GENERATOR=y

# RAM headroom (AP build ONLY): single LVGL frame buffer + smaller object pool,
# same as the STA build's proven trim.
CONFIG_LV_Z_DOUBLE_VDB=n
CONFIG_LV_Z_MEM_POOL_SIZE=80000

# Quiet the bench I2C error spam (PSU often not attached on the bench).
CONFIG_I2C_LOG_LEVEL_OFF=y
```

- [ ] **Step 3: Build for the Pico W and check RAM fit**

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico/rp2040/w -d build_ap -p auto -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```
Expected: configures as `RPI_PICO`, build completes, memory report prints with **RAM under 100%**. (No project C added yet — `APP_WIFI_AP` has no sources gated to it until Task 2.)

- [ ] **Step 4: Confirm the AP/DHCP-server/sockets config got in**

Run:
```bash
grep -E 'CONFIG_(APP_WIFI_AP|WIFI|WIFI_AIROC|CYW43439|NET_DHCPV4_SERVER|NET_SOCKETS|NET_L2_WIFI_SHELL)=y' build_ap/zephyr/.config
```
Expected: all of `CONFIG_APP_WIFI_AP=y`, `CONFIG_WIFI=y`, `CONFIG_WIFI_AIROC=y`, `CONFIG_CYW43439=y`, `CONFIG_NET_DHCPV4_SERVER=y`, `CONFIG_NET_SOCKETS=y`, `CONFIG_NET_L2_WIFI_SHELL=y` present.

- [ ] **Step 5: If — and only if — RAM overflowed in Step 3**

If Step 3 reported `region 'RAM' overflowed by N bytes`, apply the trim order, lowest-impact first, rebuild after each:
1. `CONFIG_NET_BUF_RX_COUNT=8`, `CONFIG_NET_BUF_TX_COUNT=8`, `CONFIG_NET_PKT_RX_COUNT=4`, `CONFIG_NET_PKT_TX_COUNT=4`.
2. If still over, `CONFIG_LV_Z_MEM_POOL_SIZE=72000`.
Re-run Step 3 until RAM is under 100%. Skip this step if Step 3 already fit.

- [ ] **Step 6: Flash and prove the AP comes up on hardware (the de-risk gate)**

Flash (UF2: hold BOOTSEL, copy `build_ap/zephyr/zephyr.uf2`). Open the USB-CDC serial port (e.g. `screen /dev/tty.usbmodem* 115200`). In the shell:
```
uart:~$ wifi ap enable -s "zpsu-test" -k 1 -p "zpsu1234" -c 6
```
Expected: the log prints an **AP enable success** (`AP enabled` / `AP_ENABLE_RESULT` ok). On a phone's WiFi list, **`zpsu-test` appears** and you can join it with `zpsu1234` (the phone may report "no internet / limited" — expected, there is no DHCP server running yet without the Task 2 C code; joining proves the radio/AP path). Confirm the watchface still renders.

> **STOP-AND-REPORT gate:** if `wifi ap enable` errors or the SSID never appears, the AIROC AP path does not work on this hardware/firmware — stop and report before writing any C code. (Source review says it is supported: `airoc_wifi.c:666,865`.)

- [ ] **Step 7: Commit**

```bash
cd ~/Developer/zpsu_mon
git add Kconfig ap.conf
git commit -m "feat(net): opt-in ap.conf + APP_WIFI_AP gate for Pico W SoftAP

Brings up WiFi AP + DHCPv4 server + UDP sockets for rpi_pico/rp2040/w via
-DEXTRA_CONF_FILE=ap.conf. Mutually exclusive with wifi.conf; default and
STA builds unaffected. De-risked on metal with shell 'wifi ap enable'.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `wifi_ap.c` — auto AP at boot + static IP + DHCP server

**Files:**
- Create: `src/net/wifi_ap.c`
- Create: `src/net/ap_creds.h.sample`
- Modify: `.gitignore` (add `src/net/ap_creds.h`)
- Modify: `CMakeLists.txt` (gate `wifi_ap.c`; exclude `wifi_net.c` in AP builds)

After this task the device boots straight into an AP named `zpsu-<MAC4>`, hands clients a DHCP lease in `192.168.4.0/24`, and is reachable at `192.168.4.1` — no shell commands needed.

- [ ] **Step 1: Create `src/net/ap_creds.h.sample`**

```c
/* Template for overriding the WiFi AP SSID / passphrase / channel.
 *
 * Copy to ap_creds.h (which is gitignored) and edit:
 *   cp src/net/ap_creds.h.sample src/net/ap_creds.h
 *
 * Without ap_creds.h, src/net/wifi_ap.c uses a default SSID of
 * "zpsu-<last 4 hex of the chip MAC>", passphrase "zpsu1234", channel 6.
 */
#pragma once

#define AP_SSID    "zpsu-bench"     /* omit to auto-derive "zpsu-<MAC4>" */
#define AP_PSK     "zpsu1234"       /* WPA2-PSK; >= 8 chars */
#define AP_CHANNEL 6                /* 2.4 GHz: 1..11 */
```

- [ ] **Step 2: Create `src/net/wifi_ap.c`**

```c
/*
 * Pico W WiFi Access Point glue.
 *
 * At boot: enable the SoftAP via the AIROC driver (NET_REQUEST_WIFI_AP_ENABLE),
 * then give the single WiFi net_if a static 192.168.4.1/24 and start a DHCPv4
 * server so joining clients get an address. SSID defaults to "zpsu-<MAC4>";
 * SSID/PSK/channel can be overridden by a gitignored src/net/ap_creds.h.
 *
 * AP-only: the airoc driver rejects AP-enable while a STA link is up (-EBUSY).
 * Compiled only when CONFIG_APP_WIFI_AP is set (the ap.conf opt-in fragment).
 * Design: docs/superpowers/specs/2026-06-07-pico-w-wifi-ap-design.md
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/wifi_mgmt.h>
#include <stdio.h>

/* Optional, gitignored overrides for SSID/PSK/channel. */
#if defined(__has_include)
#  if __has_include("ap_creds.h")
#    include "ap_creds.h"
#  endif
#endif

#ifndef AP_PSK
#define AP_PSK "zpsu1234"
#endif
#ifndef AP_CHANNEL
#define AP_CHANNEL 6
#endif

LOG_MODULE_REGISTER(wifi_ap, LOG_LEVEL_INF);

#define AP_RETRY_MS 5000

static struct net_mgmt_event_callback ap_cb;
static char ap_ssid[20];

static void ap_bringup_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ap_bringup, ap_bringup_fn);

/* Assign the static AP address + start the DHCP server on the WiFi iface. */
static void ap_configure_ip(struct net_if *iface)
{
	struct in_addr addr, netmask, pool_base;

	net_addr_pton(AF_INET, "192.168.4.1", &addr);
	net_addr_pton(AF_INET, "255.255.255.0", &netmask);
	net_addr_pton(AF_INET, "192.168.4.2", &pool_base);

	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_WRN("could not add static AP address");
	}
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);

	if (net_dhcpv4_server_start(iface, &pool_base) < 0) {
		LOG_WRN("DHCPv4 server failed to start (clients need static IP)");
	} else {
		LOG_INF("AP up: 192.168.4.1, DHCP pool from 192.168.4.2");
	}
}

static void ap_bringup_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = {0};
	int ret;

	if (iface == NULL) {
		k_work_reschedule(&ap_bringup, K_MSEC(AP_RETRY_MS));
		return;
	}

#if defined(AP_SSID)
	(void)snprintf(ap_ssid, sizeof(ap_ssid), "%s", AP_SSID);
#else
	struct net_linkaddr *ll = net_if_get_link_addr(iface);

	if (ll != NULL && ll->len >= 6) {
		(void)snprintf(ap_ssid, sizeof(ap_ssid), "zpsu-%02X%02X",
			       ll->addr[4], ll->addr[5]);
	} else {
		(void)snprintf(ap_ssid, sizeof(ap_ssid), "zpsu-0000");
	}
#endif

	params.ssid = (const uint8_t *)ap_ssid;
	params.ssid_length = strlen(ap_ssid);
	params.psk = (const uint8_t *)AP_PSK;
	params.psk_length = sizeof(AP_PSK) - 1;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = AP_CHANNEL;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("starting AP \"%s\" on ch %d ...", ap_ssid, AP_CHANNEL);
	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
	if (ret) {
		/* airoc first call can return -EAGAIN(-11); just retry. */
		LOG_WRN("AP enable failed (%d); retrying in %d ms", ret, AP_RETRY_MS);
		k_work_reschedule(&ap_bringup, K_MSEC(AP_RETRY_MS));
		return;
	}

	ap_configure_ip(iface);
}

static void ap_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		LOG_INF("AP enable result received");
		break;
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		LOG_WRN("AP disabled");
		break;
	default:
		break;
	}
}

static int wifi_ap_init(void)
{
	net_mgmt_init_event_callback(&ap_cb, ap_event_handler,
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
				     NET_EVENT_WIFI_AP_DISABLE_RESULT);
	net_mgmt_add_event_callback(&ap_cb);

	LOG_INF("WiFi AP glue ready; bringing AP up shortly");
	/* Give the chip/iface a moment to settle (MAC must be read in). */
	k_work_reschedule(&ap_bringup, K_SECONDS(3));
	return 0;
}

SYS_INIT(wifi_ap_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

- [ ] **Step 3: Ignore the real credentials header**

Append to `.gitignore` (it already lists `src/net/wifi_creds.h`):
```
src/net/ap_creds.h
```

- [ ] **Step 4: Gate the sources in `CMakeLists.txt`**

Replace the existing STA block (around `CMakeLists.txt:99`):
```cmake
if(CONFIG_WIFI)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c)
endif()
```
with:
```cmake
# Pico W WiFi glue. Both fragments set CONFIG_WIFI, so the AP build is
# distinguished by CONFIG_APP_WIFI_AP. STA (wifi_net.c) and AP (wifi_ap.c) are
# mutually exclusive and never co-compile. Default builds set neither symbol.
if(CONFIG_WIFI AND NOT CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c)
endif()

if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c)
  include_directories(${PROJECT_SOURCE_DIR}/src/net)
endif()
```

- [ ] **Step 5: Rebuild the AP image**

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico/rp2040/w -d build_ap -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```
Expected: `wifi_ap.c` compiles, build completes, RAM under 100%.

- [ ] **Step 6: Confirm STA glue is excluded from the AP build**

Run:
```bash
grep -c wifi_net build_ap/CMakeFiles/app.dir/* 2>/dev/null || echo "wifi_net excluded from AP build (correct)"
grep -rl wifi_ap build_ap/CMakeFiles/app.dir/ 2>/dev/null | head -1 && echo "wifi_ap included (correct)"
```
Expected: `wifi_net` excluded; `wifi_ap` present.

- [ ] **Step 7: Flash and verify AP + DHCP on hardware**

Flash `build_ap/zephyr/zephyr.uf2`. Watch the USB-CDC shell/log at boot:
```
[..] wifi_ap: starting AP "zpsu-XXXX" on ch 6 ...
[..] wifi_ap: AP up: 192.168.4.1, DHCP pool from 192.168.4.2
```
On a phone: join `zpsu-XXXX` with `zpsu1234`; confirm it receives an IP like `192.168.4.2`. From a laptop on the same AP:
```bash
ping 192.168.4.1
```
Expected: ICMP replies. The watchface still renders.

- [ ] **Step 8: Commit**

```bash
cd ~/Developer/zpsu_mon
git add src/net/wifi_ap.c src/net/ap_creds.h.sample .gitignore CMakeLists.txt
git commit -m "feat(net): Pico W SoftAP at boot + static IP + DHCPv4 server

wifi_ap.c enables the AIROC SoftAP (SSID zpsu-<MAC4>, WPA2, ch 6; override
via gitignored ap_creds.h), assigns 192.168.4.1/24, and starts a DHCP
server (pool from 192.168.4.2). CMake gates wifi_ap.c under APP_WIFI_AP and
excludes the STA wifi_net.c there. AP-only (driver rejects STA+AP).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `psu_cmd_*` thread-safe façade in `psu_service`

**Files:**
- Modify: `src/psu/psu_service.h` (add `struct psu_status` + `psu_cmd_*` decls)
- Modify: `src/psu/psu_service.cpp` (mutex + façade; route `PollWork` and LVGL callbacks through it)

The UDP server (Task 4) needs to drive `PsuController` from a third thread. `PsuController` is already touched from the LVGL thread (button callbacks) and the system workqueue (`PollWork`); its high-level read-modify-write ops (`ToggleOutput`/`ToggleMode`) are not atomic. This task funnels all high-level access through one mutex. **Always-compiled** (no WiFi dependency) — it also fixes the pre-existing UI/poll race, so verify the default build is unchanged.

- [ ] **Step 1: Declare the façade in `src/psu/psu_service.h`**

Add inside the existing `extern "C"` block, after `int psu_set_fan_rpm(int rpm);`:

```c
/* Snapshot of PSU state for the UDP STATUS command. Live V/A/W/energy come
 * from the latest poll; output/mode/cc are read on demand. */
struct psu_status {
  float volts;
  float amps;
  float watts;
  float energy_wh;
  bool output_on;
  bool mode_cc;      /* true = constant-current, false = constant-voltage */
  float cc_amps;
};

/* Thread-safe command façade. All serialize high-level PsuController access
 * through one mutex (shared with the 500 ms poll and the LVGL UI callbacks).
 * Each returns 0 on success, -1 on I2C/checksum failure. */
int psu_cmd_toggle_output(bool *now_on);   /* flips output; *now_on = new state */
int psu_cmd_set_output(bool on);           /* sets output to a specific state   */
int psu_cmd_toggle_mode(bool *now_cc);     /* flips CV/CC; *now_cc = new state   */
int psu_cmd_set_mode(bool cc);             /* sets CV/CC                          */
int psu_cmd_set_current(float amps);       /* absolute constant-current setpoint */
int psu_cmd_set_fan(int rpm);
int psu_cmd_get_status(struct psu_status *out);
```

(`<stdbool.h>` is already in scope via lvgl.h/C++; if a pure-C TU includes this header and lacks `bool`, it is `src/net/psu_udp.c`, which includes `<stdbool.h>` itself in Task 4.)

- [ ] **Step 2: Add the mutex and façade to `src/psu/psu_service.cpp`**

At file scope (just after the `LOG_MODULE_REGISTER` line, line 15), add:
```cpp
/* Serializes all high-level PsuController access: the 500 ms poll, the LVGL
 * UI callbacks, and the UDP command server (CONFIG_APP_WIFI_AP). */
K_MUTEX_DEFINE(g_cmd_mutex);
```

- [ ] **Step 3: Take the mutex in `PollWork`**

In `PollWork` (line 41), wrap the controller read + energy update. Change:
```cpp
  Controller().ReadMeasurement(&g_measurement);

  const int64_t elapsed_ms = k_uptime_delta(&g_last_sample_ms);
  const float power_w = g_measurement.volts * g_measurement.amps;
  g_energy_wh += power_w * elapsed_ms / 1000.0f / 3600.0f;
```
to:
```cpp
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  Controller().ReadMeasurement(&g_measurement);

  const int64_t elapsed_ms = k_uptime_delta(&g_last_sample_ms);
  const float power_w = g_measurement.volts * g_measurement.amps;
  g_energy_wh += power_w * elapsed_ms / 1000.0f / 3600.0f;
  k_mutex_unlock(&g_cmd_mutex);
```
(The zbus publish stays outside the lock.)

- [ ] **Step 4: Implement the façade in `src/psu/psu_service.cpp`**

Add these `extern "C"` functions just before `extern "C" int psu_service_init(void)` (line 141):
```cpp
extern "C" int psu_cmd_toggle_output(bool* now_on) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  bool enabled;
  int rc = Controller().ToggleOutput(&enabled) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  if (rc == 0 && now_on) {
    *now_on = enabled;
  }
  return rc;
}

extern "C" int psu_cmd_set_output(bool on) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  bool cur;
  int rc = -1;
  if (Controller().GetOutputEnabled(&cur)) {
    if (cur == on) {
      rc = 0;
    } else {
      bool now;
      rc = (Controller().ToggleOutput(&now) && now == on) ? 0 : -1;
    }
  }
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_toggle_mode(bool* now_cc) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  psu::OutputMode mode;
  int rc = Controller().ToggleMode(&mode) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  if (rc == 0 && now_cc) {
    *now_cc = (mode == psu::OutputMode::kConstantCurrent);
  }
  return rc;
}

extern "C" int psu_cmd_set_mode(bool cc) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  psu::OutputMode mode;
  int rc = -1;
  if (Controller().GetMode(&mode)) {
    bool is_cc = (mode == psu::OutputMode::kConstantCurrent);
    if (is_cc == cc) {
      rc = 0;
    } else {
      psu::OutputMode now;
      rc = (Controller().ToggleMode(&now) &&
            (now == psu::OutputMode::kConstantCurrent) == cc) ? 0 : -1;
    }
  }
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_set_current(float amps) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  int rc = Controller().SetConstantCurrent(amps) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_set_fan(int rpm) {
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  int rc = Controller().SetFanRpm(rpm) ? 0 : -1;
  k_mutex_unlock(&g_cmd_mutex);
  return rc;
}

extern "C" int psu_cmd_get_status(struct psu_status* out) {
  if (out == nullptr) {
    return -1;
  }
  k_mutex_lock(&g_cmd_mutex, K_FOREVER);
  out->volts = g_measurement.volts;
  out->amps = g_measurement.amps;
  out->watts = g_measurement.watts;
  out->energy_wh = g_energy_wh;

  bool enabled = false;
  Controller().GetOutputEnabled(&enabled);
  out->output_on = enabled;

  psu::OutputMode mode = psu::OutputMode::kConstantVoltage;
  Controller().GetMode(&mode);
  out->mode_cc = (mode == psu::OutputMode::kConstantCurrent);

  float cc = 0.0f;
  Controller().GetConstantCurrent(&cc);
  out->cc_amps = cc;
  k_mutex_unlock(&g_cmd_mutex);
  return 0;
}
```

- [ ] **Step 5: Route the LVGL callbacks through the façade**

Rewrite the two toggle callbacks (lines 68–87) and the current setter to go through `psu_cmd_*` (presentation/label logic stays here). Replace `psu_power_toggle_event_cb`:
```cpp
extern "C" void psu_power_toggle_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  bool enabled;
  if (psu_cmd_toggle_output(&enabled) == 0) {
    lv_label_set_text(ui_LabelOnOff, enabled ? "ON" : "OFF");
  }
}
```
Replace `psu_mode_toggle_event_cb`:
```cpp
extern "C" void psu_mode_toggle_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  bool now_cc;
  if (psu_cmd_toggle_mode(&now_cc) == 0) {
    lv_label_set_text(ui_LabelCVCC, now_cc ? "CC" : "CV");
  }
}
```
In `psu_current_set_event_cb`, the UI computes the new absolute current (its +/- stepping stays), but the read and the write now go through the façade. Replace the body's controller calls:
- change the initial `if (!Controller().GetConstantCurrent(&current)) { return; }` to:
```cpp
  struct psu_status st;
  if (psu_cmd_get_status(&st) != 0) {
    return;
  }
  float current = st.cc_amps;
```
- change the final `Controller().SetConstantCurrent(current);` to:
```cpp
  psu_cmd_set_current(current);
```
(Leave the `+= 0.5f` / long-press `5.0f` stepping and the `lv_label_set_text` exactly as they are.)

- [ ] **Step 6: Build the default (non-W) image — must be unchanged-behaviour**

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico -d build_lcd2 -p auto -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: builds clean (the façade is always-compiled; default build has no WiFi).

- [ ] **Step 7: Build the AP image too**

Run:
```bash
west build -b rpi_pico/rp2040/w -d build_ap -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```
Expected: builds clean, RAM under 100%.

- [ ] **Step 8: Flash the default build and confirm the UI still works**

Flash `build_lcd2/zephyr/zephyr.uf2` to a non-W board (or the W board — the façade is identical). Exercise the on-screen power/mode/current controls; confirm ON/OFF, CV/CC, and current up/down behave exactly as before. (Pure refactor; no user-visible change.)

- [ ] **Step 9: Commit**

```bash
cd ~/Developer/zpsu_mon
git add src/psu/psu_service.h src/psu/psu_service.cpp
git commit -m "refactor(psu): mutex-guarded psu_cmd_* facade for controller access

Serialize all high-level PsuController access (toggle/set output & mode,
set current/fan, get status) through one mutex shared by the 500 ms poll,
the LVGL UI callbacks, and (next) the UDP server. Routes the UI callbacks
through the facade, closing a pre-existing UI/poll read-modify-write race.
Always-compiled; default build behaviour unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: `psu_udp.c` — UDP command server on :5000

**Files:**
- Create: `src/net/psu_udp.c`
- Modify: `CMakeLists.txt` (add `psu_udp.c` to the `APP_WIFI_AP` block)

A dedicated thread binds UDP `:5000`, parses one text line per datagram, dispatches to `psu_cmd_*`, and replies to the sender.

- [ ] **Step 1: Create `src/net/psu_udp.c`**

```c
/*
 * UDP control server for the Pico W AP build.
 *
 * Binds 0.0.0.0:5000; each datagram is one text command, answered with one
 * text line. Dispatches to the thread-safe psu_cmd_* facade in psu_service.
 *
 *   STATUS              -> V=.. A=.. W=.. E=.. ON=0/1 MODE=CV|CC CC=..
 *   ON / OFF            -> OK ON / OK OFF
 *   MODE CV | MODE CC   -> OK MODE CV|CC
 *   CC <amps>           -> OK CC <amps>
 *   FAN <rpm>           -> OK FAN <rpm>
 *   HELP                -> verb list
 *   (anything else)     -> ERR bad command
 *
 * Compiled only when CONFIG_APP_WIFI_AP is set.
 * Design: docs/superpowers/specs/2026-06-07-pico-w-wifi-ap-design.md
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psu_service.h"

LOG_MODULE_REGISTER(psu_udp, LOG_LEVEL_INF);

#define PSU_UDP_PORT  5000
#define PSU_UDP_STACK 2048

/* Strip a trailing CR/LF and surrounding spaces in place. */
static void trim(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
			 s[n - 1] == ' ' || s[n - 1] == '\t')) {
		s[--n] = '\0';
	}
}

/* Build the reply for one command line; returns reply length. */
static int handle_cmd(char *line, char *out, size_t outsz)
{
	trim(line);

	if (strcasecmp(line, "STATUS") == 0) {
		struct psu_status st;

		if (psu_cmd_get_status(&st) != 0) {
			return snprintf(out, outsz, "ERR psu");
		}
		return snprintf(out, outsz,
				"V=%.2f A=%.2f W=%.1f E=%.3f ON=%d MODE=%s CC=%.1f",
				(double)st.volts, (double)st.amps, (double)st.watts,
				(double)st.energy_wh, st.output_on ? 1 : 0,
				st.mode_cc ? "CC" : "CV", (double)st.cc_amps);
	}
	if (strcasecmp(line, "ON") == 0) {
		return snprintf(out, outsz,
				psu_cmd_set_output(true) == 0 ? "OK ON" : "ERR psu");
	}
	if (strcasecmp(line, "OFF") == 0) {
		return snprintf(out, outsz,
				psu_cmd_set_output(false) == 0 ? "OK OFF" : "ERR psu");
	}
	if (strcasecmp(line, "MODE CV") == 0) {
		return snprintf(out, outsz,
				psu_cmd_set_mode(false) == 0 ? "OK MODE CV" : "ERR psu");
	}
	if (strcasecmp(line, "MODE CC") == 0) {
		return snprintf(out, outsz,
				psu_cmd_set_mode(true) == 0 ? "OK MODE CC" : "ERR psu");
	}
	if (strncasecmp(line, "CC ", 3) == 0) {
		float amps = strtof(line + 3, NULL);

		if (psu_cmd_set_current(amps) == 0) {
			return snprintf(out, outsz, "OK CC %.1f", (double)amps);
		}
		return snprintf(out, outsz, "ERR psu");
	}
	if (strncasecmp(line, "FAN ", 4) == 0) {
		int rpm = atoi(line + 4);

		if (psu_cmd_set_fan(rpm) == 0) {
			return snprintf(out, outsz, "OK FAN %d", rpm);
		}
		return snprintf(out, outsz, "ERR psu");
	}
	if (strcasecmp(line, "HELP") == 0) {
		return snprintf(out, outsz,
				"STATUS|ON|OFF|MODE CV|MODE CC|CC <a>|FAN <rpm>");
	}
	return snprintf(out, outsz, "ERR bad command");
}

static void psu_udp_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(PSU_UDP_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	char buf[64];
	char reply[96];
	int sock;

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return;
	}
	if (zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("bind() failed: %d", errno);
		zsock_close(sock);
		return;
	}
	LOG_INF("UDP control server on :%d", PSU_UDP_PORT);

	while (1) {
		struct sockaddr_in src;
		socklen_t slen = sizeof(src);
		int n = zsock_recvfrom(sock, buf, sizeof(buf) - 1, 0,
				       (struct sockaddr *)&src, &slen);

		if (n <= 0) {
			continue;
		}
		buf[n] = '\0';

		int rn = handle_cmd(buf, reply, sizeof(reply));

		if (rn > 0) {
			(void)zsock_sendto(sock, reply, rn, 0,
					   (struct sockaddr *)&src, slen);
		}
	}
}

K_THREAD_DEFINE(psu_udp_tid, PSU_UDP_STACK, psu_udp_thread,
		NULL, NULL, NULL, 7, 0, 0);
```

- [ ] **Step 2: Add the source to the `APP_WIFI_AP` CMake block**

In `CMakeLists.txt`, extend the AP block from Task 2 to:
```cmake
if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
    ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c)
  include_directories(${PROJECT_SOURCE_DIR}/src/net ${PROJECT_SOURCE_DIR}/src/psu)
endif()
```
(The added `src/psu` include path lets `psu_udp.c` find `psu_service.h`.)

- [ ] **Step 3: Rebuild the AP image**

Run:
```bash
cd ~/Developer/zpsu_mon
west build -b rpi_pico/rp2040/w -d build_ap -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```
Expected: `psu_udp.c` compiles, build completes, RAM under 100%. (If RAM overflows, apply Task 1 Step 5 trims.)

- [ ] **Step 4: Flash and verify the protocol with `nc`**

Flash `build_ap/zephyr/zephyr.uf2`. On a laptop, join `zpsu-XXXX`. Confirm in the boot log: `UDP control server on :5000`. Then:
```bash
printf 'STATUS\n'  | nc -u -w1 192.168.4.1 5000
printf 'ON\n'      | nc -u -w1 192.168.4.1 5000
printf 'CC 5.0\n'  | nc -u -w1 192.168.4.1 5000
printf 'MODE CC\n' | nc -u -w1 192.168.4.1 5000
printf 'OFF\n'     | nc -u -w1 192.168.4.1 5000
printf 'BOGUS\n'   | nc -u -w1 192.168.4.1 5000
```
Expected replies: a `V=.. A=.. W=.. E=.. ON=.. MODE=.. CC=..` line; `OK ON`; `OK CC 5.0`; `OK MODE CC`; `OK OFF`; `ERR bad command`. With a real PSU attached, the watchface labels and the physical output change to match `ON`/`OFF`/`MODE`/`CC`.

- [ ] **Step 5: Commit**

```bash
cd ~/Developer/zpsu_mon
git add src/net/psu_udp.c CMakeLists.txt
git commit -m "feat(net): UDP PSU control server on :5000 for the AP build

A dedicated thread binds UDP :5000 and answers a small text protocol
(STATUS/ON/OFF/MODE/CC/FAN/HELP), dispatching to the mutex-guarded
psu_cmd_* facade. AP-gated (CONFIG_APP_WIFI_AP).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Host-side smoke test + README

**Files:**
- Create: `psu_ap_smoke.py` (repo root)
- Modify: `README.md` (one build-matrix line)

- [ ] **Step 1: Create `psu_ap_smoke.py`**

```python
#!/usr/bin/env python3
"""Smoke-test the Pico W AP UDP control protocol.

Join the device's AP (zpsu-XXXX), then run:  python3 psu_ap_smoke.py
Sends each command to 192.168.4.1:5000 and checks the reply shape.
"""
import socket
import sys

HOST, PORT = "192.168.4.1", 5000


def ask(sock, cmd, timeout=1.5):
    sock.settimeout(timeout)
    sock.sendto((cmd + "\n").encode(), (HOST, PORT))
    data, _ = sock.recvfrom(128)
    return data.decode().strip()


def main():
    checks = [
        ("STATUS", lambda r: r.startswith("V=") and "ON=" in r and "CC=" in r),
        ("ON", lambda r: r == "OK ON"),
        ("CC 5.0", lambda r: r == "OK CC 5.0"),
        ("MODE CC", lambda r: r == "OK MODE CC"),
        ("OFF", lambda r: r == "OK OFF"),
        ("BOGUS", lambda r: r.startswith("ERR")),
    ]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    failures = 0
    for cmd, ok in checks:
        try:
            reply = ask(sock, cmd)
        except socket.timeout:
            print(f"FAIL {cmd!r}: no reply (timeout)")
            failures += 1
            continue
        status = "PASS" if ok(reply) else "FAIL"
        if status == "FAIL":
            failures += 1
        print(f"{status} {cmd!r} -> {reply!r}")
    print(f"\n{len(checks) - failures}/{len(checks)} checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the smoke test against the device**

With the laptop joined to `zpsu-XXXX` and the AP build flashed:
```bash
cd ~/Developer/zpsu_mon
python3 psu_ap_smoke.py
```
Expected: `6/6 checks passed` (exit 0). (`OK CC 5.0` / `OK MODE CC` etc. — the device need not have a real PSU attached for the protocol checks; with one attached, also confirm the watchface reacts.)

- [ ] **Step 3: Add one README build-matrix line**

Per the project's README style (new build target = a single build-matrix line, not a verbose section), find the existing Pico W WiFi entry and add the AP variant beside it. Run:
```bash
grep -n "rpi_pico/rp2040/w\|EXTRA_CONF_FILE" README.md
```
Then add, next to the existing `wifi.conf` build line, a parallel line:
```markdown
    # Pico W as a WiFi AP + UDP PSU control (mutually exclusive with wifi.conf)
    west build -b rpi_pico/rp2040/w -d build_ap -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```
(Join `zpsu-<MAC4>` / `zpsu1234`; `nc -u 192.168.4.1 5000` then `STATUS`/`ON`/`OFF`/`MODE CC`/`CC 5.0`/`FAN 3000`; or `python3 psu_ap_smoke.py`.)

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/zpsu_mon
git add psu_ap_smoke.py README.md
git commit -m "docs(net): document the Pico W AP build + add UDP smoke test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review (completed during planning)

- **Spec coverage:**
  - opt-in `ap.conf` + gate symbol + AP-only → Task 1 (Kconfig `APP_WIFI_AP`, `ap.conf`); driver-`-EBUSY`/secondary-iface facts → notes + Task 1 de-risk.
  - AP enable + static IP `192.168.4.1/24` + DHCP server (pool from `.2`) + SSID `zpsu-<MAC4>` + `ap_creds.h` override → Task 2.
  - `WIFI_INIT_PRIORITY=80`, USB-CDC logs (no J-Link), `-EAGAIN` retry → `ap.conf` comments + `wifi_ap.c` retry.
  - UDP server on `:5000`, text protocol incl. `STATUS` with energy, `ERR` on bad input → Task 4.
  - thread-safe `psu_cmd_*` façade + mutex shared with poll & UI; UI routed through it → Task 3.
  - CMake gates (`wifi_ap.c`/`psu_udp.c` under `APP_WIFI_AP`; `wifi_net.c` excluded) → Tasks 2 & 4.
  - RAM-fit checkpoint + trim order → Task 1 Steps 3/5 (and re-checked Tasks 2/3/4 builds).
  - default + STA builds untouched → Task 3 Step 6 (default) + the gate excluding `wifi_net.c` only in AP builds; default builds set neither symbol.
  - host smoke test + README line → Task 5.
- **Placeholder scan:** no TBD/TODO. All code blocks complete. Verified-against-tree APIs: `NET_REQUEST_WIFI_AP_ENABLE`, `airoc_mgmt_ap_enable` (`airoc_wifi.c:666,865`), `net_if_ipv4_addr_add` (`net_if.h:2423`), `net_if_ipv4_set_netmask_by_addr` (`net_if.h:2824`), `net_if_get_link_addr` (`net_if.h:1205`), `net_dhcpv4_server_start` (`dhcpv4_server.h:72`), `NET_EVENT_WIFI_AP_ENABLE_RESULT/DISABLE_RESULT` (`wifi_mgmt.h:434,438`), `zsock_*` sockets. `<MAC4>`/`XXXX`/`<ssid>` are runtime values, not plan gaps.
- **Type consistency:** `struct psu_status` fields (`volts/amps/watts/energy_wh/output_on/mode_cc/cc_amps`) are defined in Task 3 Step 1 and consumed identically in Task 4 `handle_cmd`. Façade names (`psu_cmd_toggle_output`/`set_output`/`toggle_mode`/`set_mode`/`set_current`/`set_fan`/`get_status`) match between header (Task 3 Step 1), impl (Task 3 Step 4), UI callers (Task 3 Step 5), and UDP caller (Task 4 Step 1). Gate symbol `CONFIG_APP_WIFI_AP` consistent across `Kconfig`, `ap.conf`, and `CMakeLists.txt`. `mode_cc == true` means constant-current everywhere.
```
