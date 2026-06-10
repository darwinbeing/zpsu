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
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psu_service.h"
#include "cred_parse.h"
#include "ap_config.h"
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include "dfu.h"
#include "dfu_cmd.h"
#endif

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
		return snprintf(out, outsz, "%s",
				psu_cmd_set_output(true) == 0 ? "OK ON" : "ERR psu");
	}
	if (strcasecmp(line, "OFF") == 0) {
		return snprintf(out, outsz, "%s",
				psu_cmd_set_output(false) == 0 ? "OK OFF" : "ERR psu");
	}
	if (strcasecmp(line, "MODE CV") == 0) {
		return snprintf(out, outsz, "%s",
				psu_cmd_set_mode(false) == 0 ? "OK MODE CV" : "ERR psu");
	}
	if (strcasecmp(line, "MODE CC") == 0) {
		return snprintf(out, outsz, "%s",
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
			"STATUS|ON|OFF|MODE CV|MODE CC|CC <a>|FAN <rpm>|SETAP <ssid> <psk>|DFU");
	}
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
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	if (dfu_cmd_is_trigger(line)) {
		/* Reply BEFORE the deferred reboot drops USB; the upgrade then runs
		 * inside MCUboot serial recovery (see src/dfu/dfu.c). */
		int rn = snprintf(out, outsz, "OK DFU (rebooting into recovery)");

		dfu_enter_recovery();
		return rn;
	}
#endif
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
	char buf[128];
	char reply[96];
	int sock = -1;

	/* Retry socket+bind until they succeed: the net stack / iface may not be
	 * fully up the instant this thread is first scheduled at boot. */
	while (sock < 0) {
		sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock < 0) {
			LOG_WRN("socket() failed: %d; retrying", errno);
			k_sleep(K_SECONDS(1));
			continue;
		}
		if (zsock_bind(sock, (struct sockaddr *)&bind_addr,
			       sizeof(bind_addr)) < 0) {
			LOG_WRN("bind() failed: %d; retrying", errno);
			zsock_close(sock);
			sock = -1;
			k_sleep(K_SECONDS(1));
		}
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

		/* snprintf returns the would-be length; clamp so a truncated
		 * reply never makes sendto read past reply[]. */
		if (rn >= (int)sizeof(reply)) {
			rn = (int)sizeof(reply) - 1;
		}
		if (rn > 0) {
			(void)zsock_sendto(sock, reply, rn, 0,
					   (struct sockaddr *)&src, slen);
		}
	}
}

K_THREAD_DEFINE(psu_udp_tid, PSU_UDP_STACK, psu_udp_thread,
		NULL, NULL, NULL, 7, 0, 0);
