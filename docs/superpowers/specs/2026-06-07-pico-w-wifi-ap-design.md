# Pico W (RP2040) WiFi **AP mode** + UDP control — design

- Date: 2026-06-07
- Status: **proposed** (not yet implemented).
- Builds on the STA-mode work in
  `docs/superpowers/specs/2026-06-07-pico-w-wifi-design.md` (commit `efd5e7f`).
- Target branch: `feat/pico-w-wifi-ap` (off `main`).

## Context

The Pico W WiFi today runs in **STA mode** (`wifi.conf` + `src/net/wifi_net.c`):
the board joins an existing 2.4 GHz AP as a client and obtains an IPv4 address
by DHCP. There is no way to talk to the device without an external router and
knowing its leased IP.

The ask: make the Pico W its own **Access Point** so a phone/laptop can connect
to it directly and **send control commands** to the PSU — bidirectional. The
device is a PSU monitor/controller (`zpsu_mon`); `PsuController`
(`src/psu/psu_controller.*`, driven today from the LVGL UI and a 500 ms poll)
already exposes everything a control channel needs:

- `ToggleOutput` / `GetOutputEnabled` — output on/off
- `ToggleMode` / `GetMode` — CV ⇄ CC
- `SetConstantCurrent` / `GetConstantCurrent` — current limit
- `SetFanRpm` — fan
- live V/A/W (+ accumulated energy) published on zbus `psuctrl_data_chan`

### What the Zephyr v4.2.1 tree already gives us (verified by reading source)

- **The AIROC/WHD driver implements AP mode.** `airoc_wifi.c` has
  `airoc_mgmt_ap_enable` / `airoc_mgmt_ap_disable` wired into its mgmt ops
  (`.ap_enable` at line 865). This is the #1 risk from the STA spec
  ("the airoc driver has no `status` op") — **resolved**: AP *is* supported.
- **AP and STA are mutually exclusive at runtime.** `airoc_mgmt_ap_enable`
  returns `-EBUSY` if `is_sta_connected`. It brings up a WHD *secondary
  interface* (`whd_add_secondary_interface` / `airoc_ap_if`) but exposes it on
  the **same single Zephyr `net_if`** (`net_if_get_first_wifi()`), then calls
  `net_if_dormant_off`. → **AP-only is the natural design**; STA+AP concurrency
  is out of scope.
- AP params (reused `struct wifi_connect_req_params`): `ssid` / `ssid_length`,
  `security` (`WIFI_SECURITY_TYPE_PSK` → WPA2-AES, `_NONE` → open, `_SAE` →
  WPA3), `psk` / `psk_length`, `channel` (**2.4 GHz must be 1–11**; an invalid
  channel is silently forced to 1).
- `NET_EVENT_WIFI_AP_ENABLE_RESULT` exists for completion reporting.
- **DHCPv4 server** exists: `net_dhcpv4_server_start(iface, &base_addr)` behind
  `CONFIG_NET_DHCPV4_SERVER`.

## Goal

On an **AP-mode build** of `rpi_pico/rp2040/w`, the device boots, starts a WiFi
AP, hands connecting clients an IP by DHCP, and answers a small **UDP text
command protocol** that reads and controls the PSU — with **zero impact** on the
default builds and the existing STA (`wifi.conf`) build.

### Success criteria

- AP-mode build links and **fits RP2040 RAM** (the primary risk; see Risks).
- A phone/laptop sees SSID `zpsu-<MAC4>`, connects with the WPA2 passphrase, and
  is auto-assigned an IP in `192.168.4.0/24`; the device is reachable at
  `192.168.4.1`.
- `nc -u 192.168.4.1 5000` then:
  - `STATUS` → `V=.. A=.. W=.. E=.. ON=0/1 MODE=CV|CC CC=..`
  - `ON` / `OFF` → output toggles; the **watchface label and the real PSU**
    change to match; reply `OK ON` / `OK OFF`.
  - `MODE CV|CC`, `CC <amps>`, `FAN <rpm>` → applied; `OK ...` reply.
  - unknown/malformed → `ERR <reason>`.
- The watchface UI keeps rendering throughout (display path untouched).
- A **default `rpi_pico` build** (no `ap.conf`) is byte-for-byte unaffected.

## Non-goals

- **STA + AP concurrently** — driver-rejected (`-EBUSY`) and RAM-hostile.
- **Browser / HTTP / HTML UI** — needs TCP + an HTTP server; won't fit the
  RP2040 RAM ceiling without sacrificing the watchface. Explicitly a *later*
  feature, gated on RAM headroom (e.g. moving to **Pico 2 W / RP2350, 520 KB**).
- **TCP, IPv6, TLS** — UDP only; no secure transport. The link is a local,
  WPA2-encrypted point-to-point AP, threat model is physical proximity.
- **Captive portal / WiFi provisioning** over this AP — separate feature.
- **Auth on the UDP protocol** beyond the WPA2 join — out of scope; noted as a
  known limitation.
- Any change to the default `rpi_pico` / `rpi_pico2` builds or the STA build.

## Approach

**A second opt-in conf fragment + CMake source gate, mirroring `wifi.conf`.**
All AP/DHCP-server/UDP Kconfig lives in `ap.conf`, passed on the build line; new
glue compiles only under the AP gate; default and STA builds are untouched. STA
(`wifi_net.c`) and AP (`wifi_ap.c`) are **separate files** — each owns one mode,
no shared mutable state, each understandable in isolation.

### Build target & invocation

```sh
west blobs fetch hal_infineon          # one-time (same blobs as STA)
west build -b rpi_pico/rp2040/w -d build_ap -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
```

`ap.conf` and `wifi.conf` are **mutually exclusive** — pick one `EXTRA_CONF_FILE`.
No devicetree changes (the board `.dts` already provides CYW43439 / PIO0 SPI).

### Gate symbol

`CONFIG_WIFI` is also set by the STA `wifi.conf`, so it cannot distinguish the
two builds. Introduce a dedicated app symbol the AP fragment turns on — e.g.
`CONFIG_APP_WIFI_AP=y` in `ap.conf` (a project Kconfig bool, default n). The
CMake gate keys off `CONFIG_APP_WIFI_AP`; `wifi_net.c` stays gated on plain
`CONFIG_WIFI && !CONFIG_APP_WIFI_AP` so the two never co-compile.

### Components

1. **`ap.conf`** — opt-in overlay. Start from `wifi.conf`'s RAM-trimmed base
   (`NET_IPV4=y`, `IPV6=n`, `TCP=n`, single VDB, trimmed NET_PKT/NET_BUF,
   `I2C_LOG_LEVEL_OFF`, `WIFI_INIT_PRIORITY` left at default **80**), and change:
   - `CONFIG_APP_WIFI_AP=y` (the gate).
   - Drop the DHCP **client** (`NET_DHCPV4` not needed); add the DHCP **server**
     `CONFIG_NET_DHCPV4_SERVER=y`.
   - `CONFIG_NET_SOCKETS=y` (BSD sockets for the UDP server). No TCP.
   - Keep the USB-CDC shell + `NET_L2_WIFI_SHELL` (for `wifi ap enable` during
     bring-up / de-risk) and `SHELL_LOG_BACKEND=y` (no J-Link on this board).
   - Same RAM trim levers documented in priority order; the build-and-fit
     checkpoint gates the rest.

2. **`src/net/wifi_ap.c`** (new) — compiled under `CONFIG_APP_WIFI_AP`.
   At boot (`SYS_INIT`, APPLICATION level), via a delayable work item so the
   chip/iface settle:
   - Build `wifi_connect_req_params`: SSID `zpsu-<MAC4>` (last 4 hex of the chip
     MAC, read from the iface link addr), `security = WIFI_SECURITY_TYPE_PSK`,
     PSK + channel from defaults, **overridable** by a gitignored
     `src/net/ap_creds.h` (`AP_SSID` / `AP_PSK` / `AP_CHANNEL`; template
     `ap_creds.h.sample`, mirrors the STA `wifi_creds.h` pattern). Default PSK
     `zpsu1234`, channel `6`.
   - `net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, ...)`. On `-EAGAIN`,
     retry (same transient-first-call lesson as STA join).
   - On success: assign the iface a **static IPv4** `192.168.4.1/24`
     (`net_if_ipv4_addr_add`, `NET_ADDR_MANUAL`) + set the netmask, then
     `net_dhcpv4_server_start(iface, &base)` with `base = 192.168.4.2`.
   - Register a `NET_EVENT_WIFI_AP_ENABLE_RESULT` callback to log AP up/down.

3. **`src/net/psu_udp.c`** (new) — compiled under `CONFIG_APP_WIFI_AP`.
   A dedicated thread (`K_THREAD_DEFINE`, modest stack ~1.5–2 KB): open a UDP
   socket bound to `0.0.0.0:5000`, loop on `recvfrom`, parse one text line,
   dispatch via the `psu_cmd_*` API (component 4), `sendto` the reply to the
   sender. Line parser is tiny and allocation-free (fixed `char buf[64]`).
   Protocol (case-insensitive verbs):

   | Request        | Reply                                            |
   |----------------|--------------------------------------------------|
   | `STATUS`       | `V=12.01 A=1.50 W=18.0 E=0.123 ON=1 MODE=CV CC=20.0` |
   | `ON` / `OFF`   | `OK ON` / `OK OFF`                                |
   | `MODE CV`/`CC` | `OK MODE CC`                                      |
   | `CC <amps>`    | `OK CC 5.0`                                       |
   | `FAN <rpm>`    | `OK FAN 3000`                                     |
   | `HELP`         | one-line verb list                               |
   | anything else  | `ERR bad command`                                |

   `STATUS`'s live V/A/W/E come from a cached copy of the latest
   `psuctrl_data_chan` message (a zbus observer in `psu_service` keeps the last
   sample), so the UDP path never blocks on a fresh I2C read.

4. **`src/psu/psu_service.*`** — add a small thread-safe command façade so the
   UDP thread, the LVGL callbacks, and the poll work all funnel high-level
   `PsuController` access through one place:
   - `int psu_cmd_set_output(bool on);`
   - `int psu_cmd_set_mode(bool cc);`
   - `int psu_cmd_set_current(float amps);`
   - `int psu_cmd_set_fan(int rpm);`
   - `int psu_cmd_get_status(struct psu_status *out);` (on/mode/cc + cached V/A/W/E)

   All guarded by **one `K_MUTEX`** around `PsuController` use. **Why this
   matters:** `PsuController` is *already* touched from two threads today — the
   LVGL UI thread (button callbacks) and the system workqueue (`PollWork`). The
   I2C bus driver serialises individual transfers, but multi-step read-modify-
   write ops (`ToggleOutput`, `ToggleMode`) are not atomic. The UDP thread is a
   **third** caller; the mutex closes that race for all three (the existing
   LVGL callbacks are refactored to call `psu_cmd_*` too). UI label updates
   stay on the LVGL side (lvgl is not thread-safe); the UDP thread only touches
   the controller + cached status, never LVGL.

5. **`CMakeLists.txt`** — alongside the existing `if(CONFIG_WIFI)` block:
   ```cmake
   if(CONFIG_APP_WIFI_AP)
     target_sources(app PRIVATE
       ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
       ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c)
     include_directories(${PROJECT_SOURCE_DIR}/src/net)
   endif()
   ```
   and gate the STA source so the two modes never co-compile:
   `if(CONFIG_WIFI AND NOT CONFIG_APP_WIFI_AP) → wifi_net.c`.

### Data flow

Boot → `wifi_ap.c` enables AP (WHD secondary iface) → static IP `192.168.4.1` +
DHCP server on the iface. Client joins SSID → DHCP lease `192.168.4.x`. Client
`sendto 192.168.4.1:5000 "ON\n"` → `psu_udp.c` parses → `psu_cmd_set_output(true)`
(mutex) → `PsuController` over I2C → real PSU output on; `sendto` `OK ON` back.
Meanwhile `PollWork` keeps publishing `psuctrl_data_chan`; a zbus observer caches
the latest sample for `STATUS`. The LVGL watchface renders unchanged.

### Error handling

- AP enable failure / `-EAGAIN` → logged + retried (delayable work), same as the
  STA first-join lesson.
- `net_dhcpv4_server_start` failure → logged; AP still up (clients can use a
  static IP) — no silent half-state.
- UDP parse errors → `ERR bad command` reply; never crash the thread.
- `psu_cmd_*` returns non-zero on I2C failure → `ERR psu` reply; controller
  retains last-good state (matches existing poll behaviour).
- Whole feature behind `CONFIG_APP_WIFI_AP`; omitted in default/STA builds.

## Implementation order (de-risk first)

0. **Confirm the driver actually brings up an AP on this hardware.** Minimal
   `ap.conf` (AP + shell, no UDP yet); flash; from the USB-CDC shell run
   `wifi ap enable -s "zpsu-test" -k 1 -p "zpsu1234" -c 6`; confirm a phone sees
   and joins the SSID. Source says it's supported — this proves it on metal
   before building the UDP layer. **If it fails here, stop and report.**
1. Static IP + DHCP server in `wifi_ap.c`; confirm the client gets a lease.
2. `psu_cmd_*` façade + mutex in `psu_service`; refactor LVGL callbacks onto it
   (no behaviour change — verify on the device UI first).
3. `psu_udp.c` UDP server + protocol; smoke-test with `nc -u` / a Python script.
4. RAM/fit pass + cleanup; default-build regression check.

## Testing / verification

1. Build with the command above; **links and fits** RP2040 RAM.
2. Flash; watchface still renders; USB-CDC shell prompt present.
3. Step-0 shell `wifi ap enable` proves AP on metal.
4. Phone joins `zpsu-<MAC4>`; gets `192.168.4.x`; `ping 192.168.4.1` succeeds.
5. `nc -u 192.168.4.1 5000`: `STATUS`, `ON`, `OFF`, `MODE CC`, `CC 5.0`,
   `FAN 3000` — each returns the expected reply **and** the watchface label +
   the physical PSU change to match.
6. A repo-root `psu_ap_smoke.py` (style of the existing `i2c_scan.py`) sends the
   verbs and asserts the replies.
7. **Default `rpi_pico` build** (no `ap.conf`): unaffected — new sources
   excluded, no new Kconfig in the image. STA `wifi.conf` build still works.

## Risks & fallback

- **RAM budget (primary).** STA already sits ~89 % of 264 KB. AP adds the WHD
  *secondary interface* + DHCP server + a UDP socket/thread; net of dropping the
  DHCP *client*, expected to fit but **must be measured at step 1**. Mitigation
  order (same levers as `wifi.conf`): trim `NET_PKT_*`/`NET_BUF_*`, lower
  `CONFIG_LV_Z_MEM_POOL_SIZE` *in `ap.conf` only*. If it cannot fit with the
  watchface, fallback is **Pico 2 W / RP2350 (520 KB)**.
- **WHD secondary-interface RAM** specifically may exceed STA's footprint —
  flagged for the step-1 measurement; not yet quantified.
- **Single Zephyr iface for AP.** The driver swaps `airoc_if = airoc_ap_if`
  internally but keeps one `net_if`; our static-IP + DHCP-server config attaches
  to `net_if_get_first_wifi()` — confirm the address survives AP enable ordering
  (configure **after** `AP_ENABLE_RESULT` success).
- **No transport auth.** Anyone who joins the WPA2 AP can drive the PSU. Accepted
  for the local-proximity threat model; documented. A shared-secret token on the
  UDP verbs is a possible later hardening.
- **2.4 GHz channel** must be 1–11 (driver forces 1 otherwise); default 6 is in
  range.
- **Blobs not fetched** → clear build failure; same one-time `west blobs fetch`.

## Open questions (to settle in the plan)

- Exact `psu_status` struct shape for `STATUS` (which fields, formatting/units).
- Whether `MODE`/`CC` map cleanly onto `ToggleMode`/`SetConstantCurrent` or need
  small absolute-set helpers on `PsuController` (today CC is +/- stepped from the
  UI; UDP wants an absolute `CC <amps>` — `SetConstantCurrent` already takes an
  absolute value, so this should be direct).
- DHCP server pool size (how many concurrent clients) vs. RAM — start at a small
  pool (2–4 leases).
