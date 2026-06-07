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
	/* Persist first; adopt into RAM only if BOTH keys land in NVS. A
	 * mid-write flash failure must never leave a mismatched ssid/psk pair,
	 * which would survive reboots as a silent authentication failure. */
	rc = settings_save_one("ap/ssid", new_ssid, strlen(new_ssid));
	if (rc == 0) {
		rc = settings_save_one("ap/psk", new_psk, strlen(new_psk));
		if (rc != 0) {
			/* Roll back so the two keys never diverge. */
			(void)settings_delete("ap/ssid");
		}
	}
	if (rc == 0) {
		(void)strncpy(ssid, new_ssid, sizeof(ssid) - 1);
		ssid[sizeof(ssid) - 1] = '\0';
		(void)strncpy(psk, new_psk, sizeof(psk) - 1);
		psk[sizeof(psk) - 1] = '\0';
		seeded = true;
	}
	k_mutex_unlock(&lock);

	if (rc) {
		LOG_ERR("persist AP creds failed (%d)", rc);
		return -1;
	}
	LOG_INF("AP creds updated: ssid=\"%s\"", ssid);
	return 0;
}

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
