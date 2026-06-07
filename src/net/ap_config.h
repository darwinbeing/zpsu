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

/* Defined in wifi_ap.c: trigger a live AP re-enable with the current creds. */
void wifi_ap_request_restart(void);

#endif /* AP_CONFIG_H */
