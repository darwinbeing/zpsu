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
