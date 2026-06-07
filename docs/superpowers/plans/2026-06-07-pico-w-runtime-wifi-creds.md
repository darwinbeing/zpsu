# Pico W Runtime-Configurable WiFi Credentials — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Pico W STA and SoftAP credentials settable at runtime (and persistent across reboots) instead of baked in at build time.

**Architecture:** A new flash `storage_partition` + Settings→NVS gives persistence. STA uses Zephyr v4.4.0's native `wifi_credentials` library (provisioned via the `wifi cred` shell, connected via `NET_REQUEST_WIFI_CONNECT_STORED`). AP creds live in a small `ap_config` settings module, provisioned via a new `SETAP` UDP command and an `apset` shell command. Compile-time `wifi_creds.h`/`ap_creds.h` become one-time first-boot seeds.

**Tech Stack:** Zephyr v4.4.0, NVS, Settings subsystem, `wifi_credentials`, AIROC/WHD WiFi, RP2040, west/CMake. Pure parse/validate logic is Zephyr-free C tested with host `clang`.

**Reference spec:** `docs/superpowers/specs/2026-06-07-pico-w-runtime-wifi-creds-design.md`

**Build env (every firmware build/verify step):**
```bash
export PATH=/Users/litao/anaconda3/envs/zephyr44/bin:$PATH
export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/Users/litao/Developer/zephyrproject/arm-gnu-toolchain-13.2.Rel1-darwin-x86_64-arm-none-eabi
source "$ZEPHYR_BASE/zephyr-env.sh"
cd /Users/litao/Developer/zpsu_mon
```

---

## Task 1: Flash storage partition + NVS-backed Settings foundation

Persistence does not work today (no `storage_partition`, no FS mount, no `settings_load()`). This task adds the partition, switches Settings to NVS, removes the dead FATFS config, and loads settings at boot. Scoped to the Pico W WiFi/AP builds.

**Files:**
- Create: `boards/pico_w_storage.overlay`
- Create: `src/net/persist.c`
- Modify: `CMakeLists.txt`
- Modify: `wifi.conf`, `ap.conf`
- Modify: `prj.conf:140-153`

- [ ] **Step 1: Create the storage-partition DT overlay**

Create `boards/pico_w_storage.overlay`:
```dts
/*
 * Pico W (RP2040 /w) WiFi/AP builds: carve a 64 KB storage_partition from the
 * top of the 2 MB flash for NVS-backed Settings (runtime WiFi/AP credentials).
 * The stock rpi_pico flash map is one read-only code-partition spanning all
 * usable flash; shrink it and add storage at the top. The image is ~1.2 MB, so
 * it never reaches 0x1f0000. CONFIG_SETTINGS_NVS auto-uses the node labeled
 * `storage_partition` (no chosen needed).
 */
&code_partition {
	reg = <0x100 (DT_SIZE_M(2) - 0x100 - DT_SIZE_K(64))>;
};

&flash0 {
	partitions {
		storage_partition: partition@1f0000 {
			label = "storage";
			reg = <0x001f0000 DT_SIZE_K(64)>;
		};
	};
};
```

- [ ] **Step 2: Apply the overlay for WiFi/AP builds in CMakeLists.txt**

In `CMakeLists.txt`, after the existing `DTC_OVERLAY_FILE` block (the `list(APPEND DTC_OVERLAY_FILE ... pico_display_pio.dtsi)` section near line 40), add:
```cmake
# Pico W WiFi/AP builds (wifi.conf / ap.conf) need a flash storage partition for
# NVS-backed Settings. DT is preprocessed before Kconfig, so gate on the conf
# fragment (a CMake-time -D), not CONFIG_WIFI. These builds are always the
# rpi_pico/rp2040/w board, where &flash0 / &code_partition exist.
if("${EXTRA_CONF_FILE}" MATCHES "wifi.conf" OR "${EXTRA_CONF_FILE}" MATCHES "ap.conf")
  list(APPEND DTC_OVERLAY_FILE "boards/pico_w_storage.overlay")
endif()
```

- [ ] **Step 3: Remove the dead FATFS/SETTINGS_FILE block from prj.conf**

In `prj.conf`, replace lines 140-153 (from `CONFIG_DISK_ACCESS=y` through `CONFIG_NVS=n`) — keep the `CONFIG_FLASH*` lines 135-138 — with:
```conf
# (Persistence for the Pico W WiFi/AP builds is configured in wifi.conf/ap.conf:
#  Settings -> NVS on the storage_partition. The old FATFS/SETTINGS_FILE config
#  here was never wired up — no backing disk, no mount, no settings_load.)
```

- [ ] **Step 4: Add NVS-backed Settings to wifi.conf and ap.conf**

Append to **both** `wifi.conf` and `ap.conf`:
```conf
# --- Persistent storage for runtime credentials (Settings -> NVS) ---
# Uses the storage_partition from boards/pico_w_storage.overlay.
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```
(`CONFIG_MPU_ALLOW_FLASH_WRITE=y` stays in `prj.conf` line 137 — no need to repeat it.)

- [ ] **Step 5: Create persist.c (settings init + load at boot)**

Create `src/net/persist.c`:
```c
/*
 * Persistent-storage bring-up for the Pico W WiFi/AP builds.
 *
 * Initializes the Settings subsystem and loads stored keys (WiFi credentials
 * and AP config) from NVS at boot — before the deferred WiFi auto-connect / AP
 * bring-up read them. Never blocks boot: on failure the WiFi modules fall back
 * to their defaults. Compiled only when CONFIG_SETTINGS is set (wifi/ap conf).
 */
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(persist, LOG_LEVEL_INF);

/* Run early in the APPLICATION level, before the WiFi modules
 * (CONFIG_APPLICATION_INIT_PRIORITY == 90). */
#define PERSIST_INIT_PRIORITY 10

static int persist_init(void)
{
	int ret = settings_subsys_init();

	if (ret) {
		LOG_ERR("settings_subsys_init failed (%d); using defaults", ret);
		return 0;
	}
	ret = settings_load();
	if (ret) {
		LOG_ERR("settings_load failed (%d); using defaults", ret);
		return 0;
	}
	LOG_INF("settings loaded from NVS");
	return 0;
}

SYS_INIT(persist_init, APPLICATION, PERSIST_INIT_PRIORITY);
```

- [ ] **Step 6: Compile persist.c for WiFi/AP builds**

In `CMakeLists.txt`, inside the existing `if(CONFIG_WIFI AND NOT CONFIG_APP_WIFI_AP)` block add `persist.c`, and inside the `if(CONFIG_APP_WIFI_AP)` block add `persist.c`. Concretely, change:
```cmake
if(CONFIG_WIFI AND NOT CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c)
endif()

if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
    ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c)
endif()
```
to:
```cmake
if(CONFIG_WIFI AND NOT CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c
    ${PROJECT_SOURCE_DIR}/src/net/persist.c)
endif()

if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
    ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c
    ${PROJECT_SOURCE_DIR}/src/net/persist.c)
endif()
```

- [ ] **Step 7: Build both images to verify the partition + NVS link**

Run (build env exported):
```bash
west build -b rpi_pico/rp2040/w -d build_wifi -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf 2>&1 | tail -6
west build -b rpi_pico/rp2040/w -d build_ap -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf 2>&1 | tail -6
```
Expected: both reach `Wrote ... bytes to zephyr.uf2`. Note the RAM `%age Used` for both (AP is the budget risk; record it). Also confirm the LCD build is unaffected:
```bash
west build -b rpi_pico -d build_lcd2 -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y 2>&1 | tail -3
```
Expected: links; no `storage_partition` applied (gated off).

- [ ] **Step 8: Commit**

```bash
git add boards/pico_w_storage.overlay src/net/persist.c CMakeLists.txt wifi.conf ap.conf prj.conf
git commit -m "feat(net): NVS-backed Settings foundation for Pico W (storage partition + settings_load)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Zephyr-free `cred_parse` module (SETAP parse + validation) — TDD

Pure C, no Zephyr deps, so the test compiles/runs on macOS with `clang` (native_sim is Linux-centric). Shared by `ap_config` (Task 3) and `psu_udp` (Task 5).

**Files:**
- Create: `src/net/cred_parse.h`, `src/net/cred_parse.c`
- Test: `tests/cred_parse/test_cred_parse.c`

- [ ] **Step 1: Write the failing test**

Create `tests/cred_parse/test_cred_parse.c`:
```c
/* Host test (plain clang, no Zephyr): cred_parse pure logic. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "cred_parse.h"

static void test_validate_ssid(void)
{
	assert(cred_validate_ssid("home") == 0);
	assert(cred_validate_ssid("") != 0);                  /* too short */
	char long_ssid[40];
	memset(long_ssid, 'a', sizeof(long_ssid));
	long_ssid[33] = '\0';                                 /* 33 chars */
	assert(cred_validate_ssid(long_ssid) != 0);           /* > 32 */
}

static void test_validate_psk(void)
{
	assert(cred_validate_psk("12345678") == 0);           /* min 8 */
	assert(cred_validate_psk("1234567") != 0);            /* < 8 */
	char long_psk[80];
	memset(long_psk, 'a', sizeof(long_psk));
	long_psk[64] = '\0';                                  /* 64 chars */
	assert(cred_validate_psk(long_psk) != 0);             /* > 63 */
}

static void test_setap_parse_ok(void)
{
	char ssid[33] = {0}, psk[64] = {0};

	assert(setap_parse("SETAP myssid mypass12", ssid, sizeof(ssid),
			   psk, sizeof(psk)) == 0);
	assert(strcmp(ssid, "myssid") == 0);
	assert(strcmp(psk, "mypass12") == 0);
}

static void test_setap_parse_bad(void)
{
	char ssid[33] = {0}, psk[64] = {0};

	assert(setap_parse("SETAP onlyssid", ssid, sizeof(ssid),
			   psk, sizeof(psk)) != 0);            /* missing psk */
	assert(setap_parse("SETAP a short", ssid, sizeof(ssid),
			   psk, sizeof(psk)) != 0);            /* psk < 8 */
	assert(setap_parse("NOPE x y", ssid, sizeof(ssid),
			   psk, sizeof(psk)) != 0);            /* wrong verb */
}

int main(void)
{
	test_validate_ssid();
	test_validate_psk();
	test_setap_parse_ok();
	test_setap_parse_bad();
	printf("cred_parse: all tests passed\n");
	return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails (no implementation yet)**

Run:
```bash
clang -std=c11 -Wall -I src/net tests/cred_parse/test_cred_parse.c src/net/cred_parse.c -o /tmp/tcp && /tmp/tcp
```
Expected: FAIL — `cred_parse.c`/`cred_parse.h` don't exist yet (compile error).

- [ ] **Step 3: Create the header**

Create `src/net/cred_parse.h`:
```c
/*
 * Zephyr-free credential parsing/validation helpers (host-testable).
 * WPA2 personal limits: SSID 1..32, PSK 8..63.
 */
#ifndef CRED_PARSE_H
#define CRED_PARSE_H

#include <stddef.h>

#define CRED_SSID_MIN 1u
#define CRED_SSID_MAX 32u
#define CRED_PSK_MIN  8u
#define CRED_PSK_MAX  63u

/* Return 0 if valid, -1 otherwise. */
int cred_validate_ssid(const char *ssid);
int cred_validate_psk(const char *psk);

/*
 * Parse "SETAP <ssid> <psk>" (case-insensitive verb). On success copies the
 * validated SSID/PSK into the caller buffers (NUL-terminated) and returns 0.
 * Returns -1 on any error (wrong verb, missing field, invalid length, buffer
 * too small). Input is not modified.
 */
int setap_parse(const char *line, char *ssid_out, size_t ssid_sz,
		char *psk_out, size_t psk_sz);

#endif /* CRED_PARSE_H */
```

- [ ] **Step 4: Implement cred_parse.c**

Create `src/net/cred_parse.c`:
```c
#include "cred_parse.h"
#include <string.h>
#include <strings.h>

int cred_validate_ssid(const char *ssid)
{
	size_t n = ssid ? strlen(ssid) : 0u;

	return (n >= CRED_SSID_MIN && n <= CRED_SSID_MAX) ? 0 : -1;
}

int cred_validate_psk(const char *psk)
{
	size_t n = psk ? strlen(psk) : 0u;

	return (n >= CRED_PSK_MIN && n <= CRED_PSK_MAX) ? 0 : -1;
}

int setap_parse(const char *line, char *ssid_out, size_t ssid_sz,
		char *psk_out, size_t psk_sz)
{
	if (line == NULL || ssid_out == NULL || psk_out == NULL) {
		return -1;
	}

	/* Verb */
	if (strncasecmp(line, "SETAP", 5) != 0 ||
	    (line[5] != ' ' && line[5] != '\t')) {
		return -1;
	}
	const char *p = line + 5;

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	/* SSID token */
	const char *ssid = p;

	while (*p && *p != ' ' && *p != '\t') {
		p++;
	}
	size_t ssid_len = (size_t)(p - ssid);

	if (ssid_len == 0u || ssid_len >= ssid_sz) {
		return -1;
	}

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	/* PSK token (rest of line, single token) */
	const char *psk = p;

	while (*p && *p != ' ' && *p != '\t') {
		p++;
	}
	size_t psk_len = (size_t)(p - psk);

	if (psk_len == 0u || psk_len >= psk_sz) {
		return -1;
	}

	/* Reject trailing garbage after the PSK token. */
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '\0') {
		return -1;
	}

	memcpy(ssid_out, ssid, ssid_len);
	ssid_out[ssid_len] = '\0';
	memcpy(psk_out, psk, psk_len);
	psk_out[psk_len] = '\0';

	if (cred_validate_ssid(ssid_out) != 0 ||
	    cred_validate_psk(psk_out) != 0) {
		return -1;
	}
	return 0;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
clang -std=c11 -Wall -I src/net tests/cred_parse/test_cred_parse.c src/net/cred_parse.c -o /tmp/tcp && /tmp/tcp
```
Expected: `cred_parse: all tests passed`

- [ ] **Step 6: Commit**

```bash
git add src/net/cred_parse.h src/net/cred_parse.c tests/cred_parse/test_cred_parse.c
git commit -m "feat(net): host-testable cred_parse (SETAP parse + WPA2 length validation)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `ap_config` settings module (AP creds store)

Settings-backed store for `ap/ssid` + `ap/psk` with first-boot seed from `ap_creds.h`/default. SSID unset ⇒ "use computed `zpsu-<MAC4>`".

**Files:**
- Create: `src/net/ap_config.h`, `src/net/ap_config.c`
- Modify: `CMakeLists.txt` (AP build sources)

- [ ] **Step 1: Create the header**

Create `src/net/ap_config.h`:
```c
/*
 * Runtime AP credential store (Settings -> NVS), keys "ap/ssid" and "ap/psk".
 * An empty stored SSID means "use the computed zpsu-<MAC4> default".
 */
#ifndef AP_CONFIG_H
#define AP_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include "cred_parse.h"

/* Copy the stored SSID into out (NUL-terminated). Returns true if a non-empty
 * SSID is stored, false if unset (caller should use the zpsu-<MAC4> default). */
bool ap_config_get_ssid(char *out, size_t out_sz);

/* Copy the effective PSK (stored, or the build default zpsu1234) into out. */
void ap_config_get_psk(char *out, size_t out_sz);

/* Validate, store (persist to NVS) and update the in-RAM copy. Returns 0 on
 * success, -1 on invalid input or storage error. */
int ap_config_set(const char *ssid, const char *psk);

#endif /* AP_CONFIG_H */
```

- [ ] **Step 2: Implement ap_config.c**

Create `src/net/ap_config.c`:
```c
#include "ap_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

/* Optional gitignored build-time seed (AP_SSID / AP_PSK). */
#if defined(__has_include)
#  if __has_include("ap_creds.h")
#    include "ap_creds.h"
#  endif
#endif

#ifndef AP_PSK
#define AP_PSK "zpsu1234"
#endif

LOG_MODULE_REGISTER(ap_config, LOG_LEVEL_INF);

static struct k_mutex lock;
static char ssid[CRED_SSID_MAX + 1];   /* "" => unset, use zpsu-<MAC4> */
static char psk[CRED_PSK_MAX + 1] = AP_PSK;
static bool seeded;

/* settings h_set: load "ap/ssid" and "ap/psk" from NVS. */
static int ap_settings_set(const char *name, size_t len,
			   settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	ssize_t rc;

	if (settings_name_steq(name, "ssid", &next) && !next) {
		if (len > CRED_SSID_MAX) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, ssid, len);
		if (rc >= 0) {
			ssid[rc] = '\0';
			seeded = true;
		}
		return rc < 0 ? rc : 0;
	}
	if (settings_name_steq(name, "psk", &next) && !next) {
		if (len > CRED_PSK_MAX) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, psk, len);
		if (rc >= 0) {
			psk[rc] = '\0';
		}
		return rc < 0 ? rc : 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ap_config, "ap", NULL, ap_settings_set,
			       NULL, NULL);

static int ap_config_init(void)
{
	k_mutex_init(&lock);

	/* First-boot seed from ap_creds.h, only if nothing was loaded from NVS
	 * (settings_load runs in persist.c at PERSIST_INIT_PRIORITY=10, before
	 * this APPLICATION-default-priority init). */
#if defined(AP_SSID)
	if (!seeded) {
		(void)ap_config_set(AP_SSID, AP_PSK);
		LOG_INF("seeded AP creds from ap_creds.h");
	}
#endif
	return 0;
}

SYS_INIT(ap_config_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool ap_config_get_ssid(char *out, size_t out_sz)
{
	bool have;

	k_mutex_lock(&lock, K_FOREVER);
	have = ssid[0] != '\0';
	(void)strncpy(out, ssid, out_sz - 1);
	out[out_sz - 1] = '\0';
	k_mutex_unlock(&lock);
	return have;
}

void ap_config_get_psk(char *out, size_t out_sz)
{
	k_mutex_lock(&lock, K_FOREVER);
	(void)strncpy(out, psk, out_sz - 1);
	out[out_sz - 1] = '\0';
	k_mutex_unlock(&lock);
}

int ap_config_set(const char *new_ssid, const char *new_psk)
{
	int rc;

	if (cred_validate_ssid(new_ssid) != 0 ||
	    cred_validate_psk(new_psk) != 0) {
		return -1;
	}

	k_mutex_lock(&lock, K_FOREVER);
	(void)strncpy(ssid, new_ssid, sizeof(ssid) - 1);
	ssid[sizeof(ssid) - 1] = '\0';
	(void)strncpy(psk, new_psk, sizeof(psk) - 1);
	psk[sizeof(psk) - 1] = '\0';
	seeded = true;

	rc = settings_save_one("ap/ssid", ssid, strlen(ssid));
	if (rc == 0) {
		rc = settings_save_one("ap/psk", psk, strlen(psk));
	}
	k_mutex_unlock(&lock);

	if (rc) {
		LOG_ERR("persist AP creds failed (%d)", rc);
		return -1;
	}
	LOG_INF("AP creds updated: ssid=\"%s\"", ssid);
	return 0;
}
```

- [ ] **Step 3: Add ap_config.c + cred_parse.c to the AP build**

In `CMakeLists.txt`, the `if(CONFIG_APP_WIFI_AP)` block becomes:
```cmake
if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
    ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c
    ${PROJECT_SOURCE_DIR}/src/net/persist.c
    ${PROJECT_SOURCE_DIR}/src/net/ap_config.c
    ${PROJECT_SOURCE_DIR}/src/net/cred_parse.c)
endif()
```

- [ ] **Step 4: Build the AP image to verify it links**

Run:
```bash
west build -b rpi_pico/rp2040/w -d build_ap -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf 2>&1 | tail -6
```
Expected: `Wrote ... bytes to zephyr.uf2`. Record RAM `%age Used`.

- [ ] **Step 5: Commit**

```bash
git add src/net/ap_config.h src/net/ap_config.c CMakeLists.txt
git commit -m "feat(net): ap_config settings store for runtime AP creds (seed from ap_creds.h)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: AP bring-up reads `ap_config` + live re-enable loop

`wifi_ap.c` reads SSID/PSK from `ap_config`; the bring-up thread keeps running and re-enables the AP when signalled (for `SETAP`/`apset`). All WHD AP calls stay on the one preemptible thread.

**Files:**
- Modify: `src/net/wifi_ap.c`

- [ ] **Step 1: Add the restart request API + signal**

In `src/net/wifi_ap.c`, add the include and a semaphore near the top (after the existing includes and `#include "ap_config.h"`):
```c
#include "ap_config.h"

static K_SEM_DEFINE(ap_restart_sem, 0, 1);

/* Ask the bring-up thread to re-enable the AP with the current ap_config
 * (called from psu_udp SETAP / the apset shell command). */
void wifi_ap_request_restart(void)
{
	k_sem_give(&ap_restart_sem);
}
```
Add the matching declaration to a shared spot — put it in `ap_config.h` so `psu_udp.c` can call it:
```c
/* Defined in wifi_ap.c: trigger a live AP re-enable with the current creds. */
void wifi_ap_request_restart(void);
```

- [ ] **Step 2: Build the params from ap_config and loop on restart**

Replace the body of `ap_bringup_thread` (the function under `#ifdef CONFIG_APP_WIFI_AP_AUTOSTART`) so the SSID/PSK come from `ap_config` and the thread re-enables on the semaphore. Replace the existing block that sets `params.ssid`/`params.psk` (the `#if defined(AP_SSID) ... #endif` block plus the params assignment and the single enable loop) with:
```c
	static char cur_ssid[CRED_SSID_MAX + 1];
	static char cur_psk[CRED_PSK_MAX + 1];

	for (;;) {
		/* SSID: stored value, else computed zpsu-<MAC4>. */
		if (!ap_config_get_ssid(cur_ssid, sizeof(cur_ssid))) {
			struct net_linkaddr *ll = net_if_get_link_addr(iface);

			if (ll != NULL && ll->len >= 6) {
				(void)snprintf(cur_ssid, sizeof(cur_ssid),
					       "zpsu-%02X%02X",
					       ll->addr[4], ll->addr[5]);
			} else {
				(void)snprintf(cur_ssid, sizeof(cur_ssid),
					       "zpsu-0000");
			}
		}
		ap_config_get_psk(cur_psk, sizeof(cur_psk));

		params.ssid = (const uint8_t *)cur_ssid;
		params.ssid_length = strlen(cur_ssid);
		params.psk = (const uint8_t *)cur_psk;
		params.psk_length = strlen(cur_psk);
		params.security = WIFI_SECURITY_TYPE_PSK;
		params.channel = AP_CHANNEL;
		params.band = WIFI_FREQ_BAND_2_4_GHZ;

		bool up = false;

		for (int attempt = 1; attempt <= AP_MAX_ATTEMPTS; attempt++) {
			int ret;

			LOG_INF("starting AP \"%s\" on ch %d (attempt %d/%d) ...",
				cur_ssid, AP_CHANNEL, attempt, AP_MAX_ATTEMPTS);
			ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params,
				       sizeof(params));
			if (ret == 0) {
				ap_configure_ip(iface);
				up = true;
				break;
			}
			LOG_WRN("AP enable failed (%d) on attempt %d/%d", ret,
				attempt, AP_MAX_ATTEMPTS);
			k_sleep(K_MSEC(AP_RETRY_MS));
		}
		if (!up) {
			LOG_ERR("AP enable failed after %d attempts; reboot to "
				"retry", AP_MAX_ATTEMPTS);
			return;
		}

		/* Wait for a restart request (SETAP / apset), then re-enable
		 * with the new creds — on THIS preemptible thread. */
		k_sem_take(&ap_restart_sem, K_FOREVER);
		LOG_INF("re-enabling AP with updated creds");
		(void)net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
		k_sleep(K_MSEC(AP_RETRY_MS));
	}
```
Also **delete the now-unused file-scope variable** `static char ap_ssid[WIFI_SSID_MAX_LEN + 1];` (near the top of `wifi_ap.c`) — the bring-up loop uses the local `cur_ssid` instead, so leaving it triggers an unused-variable warning.

Note: `ap_configure_ip()` already guards the "address already added" case with a warning, so calling it again after re-enable is safe.

- [ ] **Step 3: Make ap_configure_ip idempotent on re-enable**

`net_dhcpv4_server_start()` returns `-EALREADY` if already running across a re-enable. In `ap_configure_ip`, change the DHCP-start check so `-EALREADY` is not treated as failure:
```c
	int rc = net_dhcpv4_server_start(iface, &pool_base);

	if (rc < 0 && rc != -EALREADY) {
		LOG_WRN("DHCPv4 server failed to start (clients need static IP)");
	} else {
		LOG_INF("AP up: 192.168.4.1, DHCP pool from 192.168.4.2");
	}
```

- [ ] **Step 4: Build to verify it links**

Run:
```bash
west build -b rpi_pico/rp2040/w -d build_ap -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf 2>&1 | tail -6
```
Expected: links to `zephyr.uf2`.

- [ ] **Step 5: Commit**

```bash
git add src/net/wifi_ap.c src/net/ap_config.h
git commit -m "feat(net): AP bring-up reads ap_config + live re-enable on the preemptible thread

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: `SETAP` over UDP + `apset` shell command

**Files:**
- Modify: `src/net/psu_udp.c`
- Modify: `src/net/ap_config.c` (add the `apset` shell command here)

- [ ] **Step 1: Add SETAP to the UDP dispatcher and enlarge buffers**

In `src/net/psu_udp.c`, add includes near the top:
```c
#include "cred_parse.h"
#include "ap_config.h"
```
In `handle_cmd()`, add this branch before the final `return snprintf(out, outsz, "ERR bad command");`:
```c
	if (strncasecmp(line, "SETAP ", 6) == 0) {
		char ssid[CRED_SSID_MAX + 1];
		char psk[CRED_PSK_MAX + 1];

		if (setap_parse(line, ssid, sizeof(ssid), psk, sizeof(psk)) != 0) {
			return snprintf(out, outsz,
				"ERR setap (ssid 1-32, psk 8-63)");
		}
		if (ap_config_set(ssid, psk) != 0) {
			return snprintf(out, outsz, "ERR setap store");
		}
		/* Reply BEFORE the AP restart drops this client. */
		int rn = snprintf(out, outsz, "OK SETAP %s (reconnect)", ssid);

		wifi_ap_request_restart();
		return rn;
	}
```
Update the `HELP` reply string to include the new verb:
```c
	if (strcasecmp(line, "HELP") == 0) {
		return snprintf(out, outsz,
			"STATUS|ON|OFF|MODE CV|MODE CC|CC <a>|FAN <rpm>|SETAP <ssid> <psk>");
	}
```
Enlarge the RX/reply buffers in `psu_udp_thread` (a valid `SETAP` line is up to ~`6+32+1+63` = 102 bytes):
```c
	char buf[128];
	char reply[96];
```

- [ ] **Step 2: Add the `apset` shell command**

At the bottom of `src/net/ap_config.c`, add:
```c
#include <zephyr/shell/shell.h>
#include <stdlib.h>

static int cmd_apset(const struct shell *sh, size_t argc, char **argv)
{
	/* Usage: apset <ssid> <psk> */
	if (argc != 3) {
		shell_print(sh, "usage: apset <ssid> <psk>");
		return -EINVAL;
	}
	if (ap_config_set(argv[1], argv[2]) != 0) {
		shell_print(sh, "apset: invalid (ssid 1-32, psk 8-63) or store error");
		return -EINVAL;
	}
	shell_print(sh, "apset: stored ssid=\"%s\"; re-enabling AP", argv[1]);
	wifi_ap_request_restart();
	return 0;
}

SHELL_CMD_ARG_REGISTER(apset, NULL,
		       "Set SoftAP creds: apset <ssid> <psk>", cmd_apset, 3, 0);
```
The AP build already enables `CONFIG_SHELL` (via `ap.conf`/base), so no new Kconfig is needed.

- [ ] **Step 3: Build to verify it links**

Run:
```bash
west build -b rpi_pico/rp2040/w -d build_ap -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf 2>&1 | tail -6
```
Expected: links to `zephyr.uf2`. Record RAM `%age Used` (this is the fullest AP image — confirm headroom; if over budget, apply the `wifi.conf` trim order, mirrored into `ap.conf`).

- [ ] **Step 4: Commit**

```bash
git add src/net/psu_udp.c src/net/ap_config.c
git commit -m "feat(net): SETAP over UDP :5000 + apset shell to set SoftAP creds at runtime

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: STA — native `wifi_credentials` + seed-once + connect-stored

**Files:**
- Modify: `wifi.conf`
- Modify: `src/net/wifi_net.c`

- [ ] **Step 1: Enable the wifi_credentials library in wifi.conf**

Append to `wifi.conf`:
```conf
# --- Runtime STA credentials (native v4.4.0 library) ---
# `wifi cred add/delete/list` shell + NET_REQUEST_WIFI_CONNECT_STORED, persisted
# via the Settings -> NVS store configured above.
CONFIG_WIFI_CREDENTIALS=y
CONFIG_WIFI_CREDENTIALS_BACKEND_SETTINGS=y
```

- [ ] **Step 2: Un-gate the auto-connect machinery and connect from the stored store**

Today the entire auto-connect machinery in `wifi_net.c` is wrapped in
`#if defined(WIFI_AUTO_SSID)` — i.e. it only runs when creds were baked in at
build time. With runtime creds the store can be populated by `wifi cred add`,
so the machinery must **always** compile and run; only the one-time *seed* stays
gated.

(a) Add the include near the top (with the other `<zephyr/net/...>` includes):
```c
#include <zephyr/net/wifi_credentials.h>
```

(b) Replace the **whole** `#if defined(WIFI_AUTO_SSID) … #else … #endif` block
(the one defining `WIFI_AUTO_SECURITY`, `wifi_auto_connect_fn`, the
`K_WORK_DELAYABLE_DEFINE`, and `wifi_auto_connect_retry`) with this
always-compiled version:
```c
/* Security type for the optional one-time build-time seed only. */
#if defined(WIFI_AUTO_SSID) && !defined(WIFI_AUTO_SECURITY)
#define WIFI_AUTO_SECURITY WIFI_SECURITY_TYPE_PSK
#endif

static void wifi_auto_connect_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(wifi_auto_connect, wifi_auto_connect_fn);

static inline void wifi_auto_connect_retry(void)
{
	k_work_reschedule(&wifi_auto_connect, K_MSEC(WIFI_AUTO_RETRY_MS));
}

static void wifi_auto_connect_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	if (iface == NULL) {
		k_work_reschedule(&wifi_auto_connect, K_MSEC(WIFI_AUTO_RETRY_MS));
		return;
	}

	/* First boot: seed the store once from wifi_creds.h if it is empty.
	 * Runtime `wifi cred add/delete` overrides thereafter. */
	if (wifi_credentials_is_empty()) {
#if defined(WIFI_AUTO_SSID)
		ret = wifi_credentials_set_personal(
			WIFI_AUTO_SSID, sizeof(WIFI_AUTO_SSID) - 1,
			WIFI_AUTO_SECURITY, NULL, 0,
			WIFI_AUTO_PSK, sizeof(WIFI_AUTO_PSK) - 1, 0, 0, 0);
		if (ret) {
			LOG_WRN("seed of stored creds failed (%d)", ret);
		} else {
			LOG_INF("seeded stored creds from wifi_creds.h");
		}
#else
		LOG_INF("no stored WiFi — provision: "
			"wifi cred add -s <ssid> -k 1 -p <psk>");
		return; /* nothing to connect to yet; a runtime `wifi cred add`
			 * + `wifi connect` (or reboot) brings it up */
#endif
	}

	LOG_INF("connecting to stored network ...");
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);
	if (ret == -EALREADY) {
		return; /* already connected */
	}
	if (ret) {
		LOG_WRN("connect-stored request failed (%d); retrying in %d ms",
			ret, WIFI_AUTO_RETRY_MS);
		k_work_reschedule(&wifi_auto_connect, K_MSEC(WIFI_AUTO_RETRY_MS));
	}
}
```
The `wifi_event_handler` (which calls `wifi_auto_connect_retry()` on connect
failure / drop) and `ipv4_event_handler` (logs `WiFi up: … gw …`) are
unchanged.

(c) In `wifi_net_init`, replace the scheduling block:
```c
#if defined(WIFI_AUTO_SSID)
	LOG_INF("WiFi glue ready; auto-connecting to \"%s\" shortly", WIFI_AUTO_SSID);
	/* Give the chip/iface a moment to settle before the first attempt. */
	k_work_reschedule(&wifi_auto_connect, K_SECONDS(3));
#else
	LOG_INF("WiFi glue ready; connect with: wifi connect -s \"<ssid>\" -k 1 -p <psk>");
#endif
```
with (always schedule — the store may be seeded or runtime-provisioned):
```c
	LOG_INF("WiFi glue ready; connecting from stored credentials shortly "
		"(provision/override: wifi cred add -s <ssid> -k 1 -p <psk>)");
	/* Give the chip/iface a moment to settle before the first attempt. */
	k_work_reschedule(&wifi_auto_connect, K_SECONDS(3));
```

- [ ] **Step 3: Build the STA image to verify it links**

Run:
```bash
west build -b rpi_pico/rp2040/w -d build_wifi -p always -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf 2>&1 | tail -6
```
Expected: links to `zephyr.uf2`. Record RAM `%age Used`.

- [ ] **Step 4: Commit**

```bash
git add wifi.conf src/net/wifi_net.c
git commit -m "feat(net): STA uses native wifi_credentials (seed once, connect stored)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: On-hardware acceptance + docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Flash + verify STA runtime provisioning (on hardware)**

Flash `build_wifi/zephyr/zephyr.uf2` (BOOTSEL + copy). With a serial terminal on the USB-CDC console:
```
# fresh device (no seed): expect "no stored WiFi — provision: ..."
wifi cred add -s "<your-ssid>" -k 1 -p "<your-psk>"
kernel reboot cold
# expect: connecting to stored network -> WiFi associated -> WiFi up: <ip> gw <gw>
net ping -c 3 <gw>
```
Expected: after `wifi cred add` + reboot, it auto-connects and pings the gateway. Confirm creds survive a power cycle.

- [ ] **Step 2: Flash + verify AP runtime provisioning (on hardware)**

Flash `build_ap/zephyr/zephyr.uf2`. Join the default `zpsu-<MAC4>` AP (psk `zpsu1234`), then:
```bash
# from a host on the AP
printf 'SETAP myaphotspot mypass1234\n' | nc -u -w1 192.168.4.1 5000
# expect: "OK SETAP myaphotspot (reconnect)" then the AP drops; rejoin SSID
# "myaphotspot" with psk "mypass1234", and:
printf 'STATUS\n' | nc -u -w1 192.168.4.1 5000
```
Expected: AP comes back up with the new SSID/PSK; `STATUS` answers. Power-cycle and confirm it boots straight into `myaphotspot` (persisted). Also verify `apset myaphotspot mypass1234` over the USB-CDC shell behaves the same.

- [ ] **Step 3: Document provisioning in README.md**

In `README.md`, under the Pico W build lines, add (indented block, matching the file's style):
```
    Runtime WiFi credentials (persist in flash, no rebuild):
      STA: connect over the USB-CDC shell, then reboot —
        wifi cred add -s "<ssid>" -k 1 -p "<psk>"
      AP:  set over UDP :5000 (or the `apset` shell) while joined to the AP —
        printf 'SETAP <ssid> <psk>\n' | nc -u 192.168.4.1 5000
      Optional build-time seed: src/net/wifi_creds.h / ap_creds.h (used once on
      first boot when the store is empty).
```

- [ ] **Step 4: Capture a provisioning log as evidence (optional)**

Use `capture_wifi_boot.py` (or a serial terminal) to capture an STA boot-from-stored-creds + `net ping`, and an AP `SETAP` round-trip. Keep for the PR description.

- [ ] **Step 5: Commit**

```bash
git add README.md
git commit -m "docs: runtime WiFi/AP credential provisioning (wifi cred / SETAP)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes / risks

- **RAM budget (the main risk):** AP build is ~87% before this work; `wifi_credentials` + NVS + new modules add a few KB. If any AP/WiFi build overflows RAM, apply the trim order documented in `wifi.conf` (NET_PKT/BUF counts, `LV_Z_MEM_POOL_SIZE`, `LV_Z_DOUBLE_VDB=n`) — mirror the needed trims into `ap.conf`. Each build step records RAM% so this surfaces early.
- **CMake overlay gate:** `EXTRA_CONF_FILE` matching `wifi.conf`/`ap.conf` is the WiFi/AP signal (DT is preprocessed before Kconfig). If a future build passes the conf differently, revisit Task 1 Step 2.
- **`settings_save_one` key vs handler tree:** the handler subtree is `"ap"`; keys are saved as `"ap/ssid"`/`"ap/psk"` and matched as `"ssid"`/`"psk"` in `ap_settings_set` — keep these consistent if renamed.
- **Init ordering:** `persist_init` (priority 10) runs before `ap_config_init` and the WiFi modules (priority 90); `wifi_credentials` and `ap_config` use compile-time static settings handlers, so `settings_load()` populates them regardless of SYS_INIT order.
</content>
