# Pico W (RP2040) **runtime-configurable WiFi credentials** (STA + AP) — design

- Date: 2026-06-07
- Status: **proposed** (not yet implemented).
- Builds on:
  - STA: `docs/superpowers/specs/2026-06-07-pico-w-wifi-design.md`
  - AP + UDP control: `docs/superpowers/specs/2026-06-07-pico-w-wifi-ap-design.md`
- Zephyr **v4.4.0** (the tree was upgraded from v4.2.1; WiFi/AIROC SPI is now
  upstream-native).
- Target branch: `feat/pico-w-runtime-creds` (off `main`).

## Context

Today both WiFi roles bake their credentials in at **build time**:

- **STA** (`wifi.conf` + `src/net/wifi_net.c`): SSID/PSK come from a gitignored
  `src/net/wifi_creds.h` (`WIFI_AUTO_SSID`/`WIFI_AUTO_PSK`), pulled in via
  `__has_include`. Changing the home network ⇒ rebuild + reflash.
- **AP** (`ap.conf` + `src/net/wifi_ap.c`): SSID/PSK from `AP_SSID`/`AP_PSK`
  macros (gitignored `src/net/ap_creds.h`) or the default `zpsu-<MAC4>` /
  `zpsu1234`. Same rebuild-to-change problem.

**The ask:** set STA and AP credentials at **runtime**, persisted across
reboots, without rebuilding.

### What v4.4.0 / the codebase already give us (verified by reading source)

- **Native Wi-Fi credentials library** — `subsys/net/lib/wifi_credentials/`:
  backends `settings` / `psa` / `none`; `CONFIG_WIFI_CREDENTIALS_MAX_ENTRIES=2`;
  `wifi cred add/delete/list` shell (`WIFI_CREDENTIALS_SHELL`, default y);
  `NET_REQUEST_WIFI_CONNECT_STORED` (`WIFI_CREDENTIALS_CONNECT_STORED`,
  default y). Public API: `wifi_credentials_set_personal()`,
  `wifi_credentials_get_by_ssid_personal_struct()`,
  `wifi_credentials_delete_by_ssid()`, `wifi_credentials_for_each_ssid()`.
- **`psu_udp.c`** already has a clean line-oriented text dispatcher on UDP
  `:5000` (`STATUS/ON/OFF/MODE/CC/FAN/HELP`) — easy to extend. RX buffer is
  currently `buf[64]`.
- **Persistence is NOT actually wired up.** `prj.conf` enables
  `CONFIG_SETTINGS`/`SETTINGS_FILE`/FATFS/`DISK_ACCESS`, but: the default
  `rpi_pico` flash map has **no `storage_partition`** (one read-only
  `code-partition` spans all usable flash), no filesystem is mounted, and no
  app code calls `settings_subsys_init()`/`settings_load()`. So this config is
  **vestigial** and persistence is non-functional today. This feature
  introduces the first working persistent store.

## Requirements / decisions (from brainstorming)

1. **Scope: both STA and AP** become runtime-configurable + persistent.
2. **Provisioning channels:** STA via the native `wifi cred` shell over USB-CDC;
   AP via a new `SETAP` command on UDP `:5000` **and** an `apset` shell command.
3. **Keep compile-time creds as a one-time seed:** if `wifi_creds.h` /
   `ap_creds.h` are present at build, they seed the empty store on first boot;
   runtime changes override thereafter. The store is the source of truth.
4. **AP always works out of the box:** if nothing is stored, AP falls back to
   `zpsu-<MAC4>` / `zpsu1234`.
5. **Approach A** (chosen): native `wifi_credentials` for STA + a small custom
   `ap_config` module for AP, both persisted via **Settings → NVS**.

## Architecture & components

Scoped to the **Pico W WiFi/AP builds** only — config lives in
`wifi.conf` / `ap.conf` + a Pico W DT overlay; the LCD builds are untouched.
The dead `DISK_ACCESS`/FATFS/`SETTINGS_FILE` lines are removed from `prj.conf`.

**Storage foundation (shared by STA + AP):**
- **DT overlay** for board `rpi_pico/rp2040/w`: redefine the `&flash0`
  partitions to shrink `code-partition` and add a `storage_partition`
  (~64 KB at the top of the 2 MB flash; image is ~1.2 MB so no conflict).
  `CONFIG_SETTINGS_NVS` uses the `storage_partition` DT label by default.
- **Kconfig** (in `wifi.conf` + `ap.conf`): `CONFIG_NVS=y`,
  `CONFIG_SETTINGS_NVS=y`, `CONFIG_FLASH_MAP=y` (drop `SETTINGS_FILE`).
- **`src/net/persist.c`** (~25 lines): one early init hook →
  `settings_subsys_init()` + `settings_load()`, ordered **before** the WiFi
  modules read credentials.

**STA — `wifi_credentials` + `wifi_net.c`:**
- `wifi.conf`: `CONFIG_WIFI_CREDENTIALS=y` + `…_BACKEND_SETTINGS=y`
  (shell + connect-stored default-on).
- `wifi_net.c`: on the existing auto-connect work, if the store is empty and
  `wifi_creds.h` defines creds → `wifi_credentials_set_personal()` once; then
  issue `NET_REQUEST_WIFI_CONNECT_STORED` instead of the macro-based connect.
  Existing `CONNECT_RESULT`→DHCP, retry/backoff, and reconnect-on-drop logic
  is unchanged.
- Provisioning UX: native `wifi cred add/delete/list` — **no new code**.

**AP — new `ap_config` + `wifi_ap.c` / `psu_udp.c`:**
- **`src/net/ap_config.{c,h}`** (~50 lines): a `settings` handler for keys
  `ap/ssid`, `ap/psk`; `ap_config_load()` (seed-once from `ap_creds.h`/defaults),
  `ap_config_get_ssid/psk()`, `ap_config_set(ssid, psk)` (validate + persist).
  Mutex-guarded. An **unset** SSID means "use the computed `zpsu-<MAC4>`".
- `wifi_ap.c`: read SSID/PSK from `ap_config` instead of the `AP_SSID`/`AP_PSK`
  macros; if SSID unset, compute `zpsu-<MAC4>` at bring-up as today.
- `psu_udp.c`: add `SETAP <ssid> <psk>`; bump RX buffer 64→128 B.
- A small `apset ssid <x>` / `apset psk <y>` shell command (parity with UDP).

**Module boundaries:** `persist` (settings init) → `wifi_credentials` (STA
store, native) and `ap_config` (AP store, custom) → consumed by `wifi_net.c` /
`wifi_ap.c` / `psu_udp.c`. Each store is independently testable.

## Data flows

**STA boot:**
1. `persist` → `settings_load()` → `wifi_credentials` populates its RAM index.
2. Auto-connect work (~3 s after boot): if store empty **and** seed present →
   `wifi_credentials_set_personal(seed)` once; issue
   `NET_REQUEST_WIFI_CONNECT_STORED` → `CONNECT_RESULT` → DHCPv4 → `WiFi up: …`.
   If nothing stored → log `provision: wifi cred add -s <ssid> -k 1 -p <psk>`
   and idle, re-checking periodically.
3. Runtime: `wifi cred add/delete/list` over USB-CDC → persists → next boot
   auto-connects.

**AP boot:**
1. `persist` → `ap_config_load()` reads `ap/ssid`,`ap/psk`; first boot seeds
   from `ap_creds.h` if defined, else leaves SSID unset (PSK default
   `zpsu1234`).
2. `ap_bringup_thread` (preemptible): SSID = stored, else computed
   `zpsu-<MAC4>` → `AP_ENABLE` → static IP + DHCP server (today's path).

**AP provision (`SETAP`):**
- Client on the AP sends UDP `SETAP <ssid> <psk>` to `:5000` (or
  `apset …` over shell). `psu_udp.c` parses, validates SSID 1–32 / PSK 8–63 →
  `ap_config_set()` persists to NVS → replies `OK SETAP <ssid>` **first**, then
  gives a `k_sem` to the bring-up thread.
- The bring-up thread (now a loop: *bring up → block on sem → `AP_DISABLE` +
  `AP_ENABLE` with new params*) re-enables the AP. All WHD AP calls stay on
  that **one preemptible thread** — honoring the cooperative-workqueue wedge
  root-cause from the AP spec. **Caveat:** the provisioning client is dropped
  and must rejoin with the new SSID/PSK.

## Error handling & edge cases

- **Persistence broken at boot** (NVS init/partition error): log + fall back to
  defaults — the device must always boot and bring up the AP control hotspot.
- **Seed idempotency:** seed only when the store is empty; never overwrite a
  runtime change with the compile-time seed on later boots.
- **Persist-before-apply:** `SETAP` writes NVS *then* re-enables. If the live
  re-enable fails, the new creds are already saved → reboot brings them up.
  Bring-up retry stays bounded (as today); never an unbounded loop.
- **`SETAP` validation:** SSID 1–32, PSK 8–63 (WPA2 min); reject over-long
  lines (the 128 B buffer fits the longest valid `SETAP`) → `ERR setap <reason>`,
  store untouched.
- **`CONNECT_STORED` with empty store** → same path as a failed connect today
  (log + idle, no crash).
- **Concurrency:** `ap_config` has its own mutex (UDP/shell write vs. bring-up
  read); NVS writes are serialized by the settings subsystem; `psu_cmd_*` is
  already mutex-guarded.
- **Security (documented, not enforced):** anyone on the AP can `SETAP`.
  Acceptable for a bench tool; future option: require the current PSK. Out of
  scope.

## RAM / flash budget

`wifi_credentials` + NVS cache + new modules add a few KB. Today WiFi ~84% /
**AP ~87% RAM** — the AP build is the squeeze. Mitigation: `wifi.conf` already
documents a trim order (NET_PKT/BUF counts, `LV_Z_MEM_POOL_SIZE`, double-VDB).
**Gate:** both images must link with headroom; report actual RAM% and trim if
needed. Flash: +64 KB partition + lib against ~800 KB free — ample.

## Testing

- **Native `ztest` (host)** for the pure, radio-free logic (factor it out to be
  testable): `SETAP` line parse, SSID/PSK validation bounds, and the
  seed-decision ("empty → seed once / non-empty → skip"). The project has no
  test harness today, so this adds a minimal one.
- **On-hardware acceptance (manual)** — WiFi needs the chip (no J-Link):
  - STA: fresh flash → `wifi cred add` → reboot → auto-connect → `net ping gw`;
    change network → `wifi cred delete/add` → reboot → joins new.
  - AP: boot → default `zpsu-<MAC4>` up → `SETAP myap mypass123` → rejoin with
    new creds → `STATUS`; power-cycle → creds persist.
  - Capture a short boot+provision log as evidence (like the README capture).

## Out of scope (YAGNI)

Captive portal / web UI, PSA-encrypted creds, `SETAP` auth, WPA3-only flows,
multi-AP, STA+AP concurrency.

## File-by-file change list

- `boards/rpi_pico_rp2040_w.overlay` (new): `storage_partition` carve-out.
- `wifi.conf`: `WIFI_CREDENTIALS` (+settings backend), `NVS`, `SETTINGS_NVS`.
- `ap.conf`: `NVS`, `SETTINGS_NVS`, settings for `ap_config`.
- `prj.conf`: remove vestigial `DISK_ACCESS`/FATFS/`SETTINGS_FILE`.
- `src/net/persist.c` (new): settings init/load at boot.
- `src/net/ap_config.{c,h}` (new): AP creds store (settings-backed).
- `src/net/wifi_net.c`: seed-once + `CONNECT_STORED`.
- `src/net/wifi_ap.c`: read creds from `ap_config`; bring-up thread becomes a
  re-enable loop.
- `src/net/psu_udp.c`: `SETAP` command + 128 B buffer.
- `apset` shell command (in `ap_config.c` or a small `src/net/ap_shell.c`).
- `CMakeLists.txt`: compile the new sources under the WiFi/AP guards.
- `tests/ap_config/` (new): native ztest for parse/validate/seed logic.
</content>
