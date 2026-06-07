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
