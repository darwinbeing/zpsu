# Pico W (RP2040) WiFi connectivity — design

- Date: 2026-06-07
- Status: approved (design); implementation pending
- Branch: `feat/pico-w-wifi`

## Context

The watt meter currently builds for the plain Raspberry Pi Pico (`rpi_pico`,
RP2040) and Pico 2 (`rpi_pico2`, RP2350) with a Pico Display Pack. The board
has no networking. The README already gestures at a Pico W variant (the
`pico_w.png` image), and the goal of this change is to bring the **Pico W's
Infineon CYW43439 WiFi up to a working network connection** — nothing more.

Zephyr v4.2.1 (the tree this project builds against) has full Pico W support:

- Board target `rpi_pico/rp2040/w` (the `w` variant of the `rp2040` SoC).
- WiFi chip driven by the in-tree `infineon,airoc-wifi` driver
  (`CONFIG_WIFI_AIROC`, selected when the board's `airoc-wifi` DT node is
  enabled; the board defconfig sets `CONFIG_CYW43439=y`).
- The chip talks to the host over a **PIO-SPI bus on PIO0**
  (`raspberrypi,pico-spi-pio`), using GPIO 23/24/25/29 — all internal to the
  Pico W module, so **none of them collide with the Display Pack header pins**.
- Requires Infineon binary firmware blobs, fetched once with
  `west blobs fetch hal_infineon` (not currently present in the tree).

The console and shell are **already wired to USB CDC ACM** in every pack
overlay (`zephyr,console = &usb_cdc; zephyr,shell-uart = &usb_cdc;` with a
`usb_cdc` `zephyr,cdc-acm-uart` node under `zephyr_udc0`), and the USB device
stack initialises at boot (`CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y`). So the
interactive shell needs almost no new wiring — only `CONFIG_SHELL=y`.

## Goal

Bring up the CYW43439, associate with a 2.4 GHz AP, obtain an IPv4 address by
DHCP, and make that **verifiable interactively** — the smallest useful,
testable networking foundation, with zero impact on the existing non-W builds.

Success criteria:
- On a `rpi_pico/rp2040/w` build, the USB CDC shell offers `wifi` and `net`
  commands.
- `wifi connect "<ssid>" <psk>` associates with the AP.
- The device then auto-starts DHCPv4 and **logs the acquired IP + gateway**
  over the existing RTT log channel.
- `net iface` shows the bound address; `net ping <host>` succeeds.

## Non-goals

- NTP / SNTP time sync, telemetry push (MQTT/HTTP), web UI — explicitly later
  features; this change is connectivity only.
- Persisted credentials — SSID/PSK are entered at runtime via the shell each
  boot. No secrets in the repo, no settings-storage glue.
- TLS / secure sockets.
- WiFi + the opt-in PIO+DMA display driver together — both want **PIO0**
  (see Constraints). The WiFi build stays on the default PL022 display path.
- BT / WiFi coexistence — BT is off in this project (`CONFIG_BT` is commented
  out), so there is nothing to coexist with.
- Any change to the default `rpi_pico` / `rpi_pico2` builds.

## Approach

**Opt-in conf fragment + CMake source gate (mirrors the `DISPLAY_SPI_PIO_DMA`
pattern).** All WiFi/networking/shell Kconfig lives in one overlay fragment the
user passes on the build line; a small networking glue module compiles only
when `CONFIG_WIFI` is set; the default builds are untouched.

### Build target & invocation

```sh
west blobs fetch hal_infineon          # one-time: CYW43439 firmware blobs
west build -b rpi_pico/rp2040/w -d build_wifi -- \
      -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf
```

`CMakeLists.txt` already extracts `BOARD_NAME` via `^[^/]+`, so
`rpi_pico/rp2040/w` reduces to `rpi_pico` and the existing pack-overlay
selection (`boards/rpi_pico/pico_display_pack2.overlay`) still fires. The
board's own `rpi_pico_rp2040_w.dts` provides the CYW43439 / PIO0 SPI node, so
**no devicetree changes are needed from this project.**

### Components

1. **`wifi.conf`** — the opt-in overlay fragment (passed via `EXTRA_CONF_FILE`).
   Modelled on Zephyr's `samples/net/wifi/shell/prj.conf`, trimmed for RP2040
   RAM:
   - WiFi/net core: `CONFIG_WIFI=y`, `CONFIG_NETWORKING=y`,
     `CONFIG_NET_IPV4=y`, `CONFIG_NET_IPV6=n`, `CONFIG_NET_DHCPV4=y`,
     `CONFIG_NET_TCP=n` (foundation needs only DHCP + ICMP).
   - Shell: `CONFIG_SHELL=y`, `CONFIG_NET_SHELL=y`,
     `CONFIG_NET_L2_WIFI_SHELL=y`, `CONFIG_SHELL_LOG_BACKEND=n` (keep logs on
     RTT so the shell terminal stays clean for `wifi`/`net` interaction).
   - Buffers/stacks: modest `CONFIG_NET_PKT_*` / `CONFIG_NET_BUF_*` counts,
     `CONFIG_MAIN_STACK_SIZE` / `CONFIG_SHELL_STACK_SIZE` /
     `CONFIG_NET_TX_STACK_SIZE` / `CONFIG_NET_RX_STACK_SIZE` bumps, and
     `CONFIG_NET_MGMT_EVENT_QUEUE_SIZE`. (The project's `prj.conf` sets
     `MAIN_STACK_SIZE=2048`; the fragment raises it for the net stack.)
   - May lower `CONFIG_LV_Z_MEM_POOL_SIZE` **inside this fragment only** if the
     image does not fit RAM — without touching the default build.
   - Logging: `CONFIG_WIFI_LOG_LEVEL_ERR`, `CONFIG_NET_LOG` at a quiet level.

2. **`src/net/wifi_net.c` (+ `wifi_net.h`)** — compiled only `if(CONFIG_WIFI)`.
   ~40 lines: registers two `net_mgmt` callbacks at boot —
   - on **`NET_EVENT_WIFI_CONNECT_RESULT`** (L2 associated) →
     `net_dhcpv4_start(iface)`;
   - on **`NET_EVENT_IPV4_ADDR_ADD`** (lease bound) → log
     `WiFi up: <ip> gw <gw>`.
   This is the verification surface and the natural hook point for later
   features (NTP/telemetry). Initialised via a `SYS_INIT`/app hook so `main`
   stays untouched where possible.

3. **`CMakeLists.txt`** — add, gated on the same symbol the fragment turns on:
   ```cmake
   if(CONFIG_WIFI)
     target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c)
     include_directories(${PROJECT_SOURCE_DIR}/src/net)
   endif()
   ```

### Data flow

USB CDC shell `wifi connect` → AIROC driver associates the CYW43439 over PIO0
SPI → `NET_EVENT_WIFI_CONNECT_RESULT` → `wifi_net.c` starts DHCPv4 →
`NET_EVENT_IPV4_ADDR_ADD` → IP logged over RTT. `net ping` / `net iface`
exercise and inspect the live interface. The display/LVGL path and PSU
monitoring run unchanged alongside.

### Error handling

- WiFi association failures surface through the `wifi` shell command's own
  result reporting and the connect-result callback (status logged).
- DHCP timeout: the IPv4-bound callback simply never fires; `net iface` shows
  no address — diagnosable from the shell. No silent partial state in our glue.
- The whole feature is behind `CONFIG_WIFI`; if the fragment is omitted (the
  default and the non-W builds), none of this code compiles or runs.

## Testing / verification

1. `west blobs fetch hal_infineon`; build with the command above; confirm it
   **links and fits** RP2040 RAM (the primary risk — see below).
2. Flash the Pico W; confirm the watchface still renders (display path
   unaffected) and the USB CDC serial port enumerates with a shell prompt.
3. `wifi scan` lists nearby APs.
4. `wifi connect "<ssid>" <psk>` → watch the RTT log for `WiFi up: <ip> gw …`.
5. `net iface` shows the bound IPv4 address; `net ping <gateway-or-host>`
   succeeds.
6. Confirm a **default** `rpi_pico` build (no `wifi.conf`) is byte-for-byte
   unaffected — `wifi_net.c` excluded, no new Kconfig in the image.

## Risks & fallback

- **RAM budget (primary risk).** RP2040 has 264 KB SRAM; the existing LVGL
  config already uses a 95 KB pool + 33 KB heap + double VDB. The networking
  stack + WiFi driver add tens of KB. Mitigation order if it does not fit:
  (1) `CONFIG_NET_TCP=n` + `IPV6=n` (already chosen), (2) trim
  `NET_PKT_*`/`NET_BUF_*` counts, (3) lower `CONFIG_LV_Z_MEM_POOL_SIZE` in
  `wifi.conf` only. The build-and-fit checkpoint (step 1) gates everything.
- **PIO0 contention.** WiFi owns PIO0. The opt-in `DISPLAY_SPI_PIO_DMA` driver
  also uses a PIO, so the two are mutually exclusive for now; the WiFi build
  uses the default PL022 display path. Documented as a non-goal.
- **Blobs not fetched.** Build fails clearly if `west blobs fetch hal_infineon`
  was not run; called out in the build steps.
- **GP25 / onboard LED** is owned by the CYW43 on the Pico W. The project's
  RGB status LEDs are on the Display Pack (PWM), not GP25, so existing LED code
  is unaffected. Note (found during implementation): the W board's `.dts`
  omits Zephyr's `rpi_pico-led.dtsi`, so the `pwm_led0` label it defines does
  not exist on the W board. The pack2 overlay must reference the backlight via
  the project's own `display_blk` label instead of `&pwm_led0` (a no-op on the
  plain Pico). Fixed in `boards/rpi_pico/pico_display_pack2.overlay`.

## Open questions (resolve during implementation)

- Exact `net_mgmt` event mask constants and whether DHCPv4 is better kicked
  from the connect-result callback vs. `CONFIG_NET_CONFIG_SETTINGS` auto-init.
- Final trimmed buffer/stack counts that both fit RAM and pass `net ping`.
- Whether `wifi_net.c` registers via `SYS_INIT` or a call from `app::App::Start`.
