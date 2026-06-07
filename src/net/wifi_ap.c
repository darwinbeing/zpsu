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
#include <string.h>

/* Optional, gitignored overrides for SSID/PSK/channel. */
#if defined(__has_include)
#  if __has_include("ap_creds.h")
#    include "ap_creds.h"
#  endif
#endif

#include "ap_config.h"

static K_SEM_DEFINE(ap_restart_sem, 0, 1);

/* Ask the bring-up thread to re-enable the AP with the current ap_config
 * (called from psu_udp SETAP / the apset shell command). */
void wifi_ap_request_restart(void)
{
	k_sem_give(&ap_restart_sem);
}

#ifndef AP_PSK
#define AP_PSK "zpsu1234"
#endif
#ifndef AP_CHANNEL
#define AP_CHANNEL 6
#endif

LOG_MODULE_REGISTER(wifi_ap, LOG_LEVEL_INF);

/*
 * ROOT-CAUSE NOTE (verified on hardware): AP-enable must run from a PREEMPTIBLE
 * thread, after the CYW43439 WLAN core has settled.
 *
 * AP-enable issues a burst of WHD IOCTLs; each waits up to 5 s for the chip's
 * response, delivered over SPI by the interrupt-driven, high-priority WHD bus
 * thread. Run from the *cooperative* system workqueue (priority -1, never
 * preempted once running), that interleaving is starved/misordered and an IOCTL
 * passes its 5 s timeout — which PERMANENTLY wedges WHD's request/response
 * pipeline ("send event_msgs(iovar) failed" + "Received a response for a
 * different IOCTL"). It never recovers, so retrying is futile. The Zephyr
 * `wifi ap enable` shell command works precisely because it runs on a
 * preemptible thread. Empirically: cooperative sysworkq wedges at both 3 s and
 * 20 s; a preemptible call ~20 s in succeeds. So we drive AP-enable from a
 * dedicated preemptible thread after a settle delay, with only a small bounded
 * retry (never an unbounded loop).
 */
/* Condition-based bring-up: rather than a fixed "settle" delay, start soon and
 * retry the AP-enable until the WLAN core accepts it (ret == 0). On the
 * PREEMPTIBLE thread an early attempt SHOULD just fail cleanly while the core
 * comes up, so a later retry succeeds — no magic delay. Bounded so a genuine
 * wedge (watch for "event_msgs failed"/"different IOCTL") can't thrash forever. */
#define AP_START_DELAY_MS 3000   /* first attempt soon after boot */
#define AP_RETRY_MS       3000   /* gap between attempts while the core comes up */
#define AP_MAX_ATTEMPTS   12     /* ceiling (~tens of s), then stop — never unbounded */
#define AP_THREAD_STACK   4096   /* WHD ap-enable call chain is deep */
#define AP_THREAD_PRIO    7      /* PREEMPTIBLE (positive) — must NOT be cooperative */

static struct net_mgmt_event_callback ap_cb;

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

	int rc = net_dhcpv4_server_start(iface, &pool_base);

	if (rc < 0 && rc != -EALREADY) {
		LOG_WRN("DHCPv4 server failed to start (clients need static IP)");
	} else {
		LOG_INF("AP up: 192.168.4.1, DHCP pool from 192.168.4.2");
	}
}

#ifdef CONFIG_APP_WIFI_AP_AUTOSTART
static void ap_bringup_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	struct wifi_connect_req_params params = {0};
	struct net_if *iface;

	/* Let the WLAN core settle before the first AP-enable (see ROOT-CAUSE NOTE). */
	k_sleep(K_MSEC(AP_START_DELAY_MS));

	iface = net_if_get_first_wifi();
	for (int i = 0; iface == NULL && i < AP_MAX_ATTEMPTS; i++) {
		k_sleep(K_MSEC(AP_RETRY_MS));
		iface = net_if_get_first_wifi();
	}
	if (iface == NULL) {
		LOG_ERR("no WiFi interface found; AP not started");
		return;
	}

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
}

K_THREAD_DEFINE(ap_bringup_tid, AP_THREAD_STACK, ap_bringup_thread,
		NULL, NULL, NULL, AP_THREAD_PRIO, 0, 0);
#endif /* CONFIG_APP_WIFI_AP_AUTOSTART */

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

#ifdef CONFIG_APP_WIFI_AP_AUTOSTART
	/* The AP comes up from the dedicated preemptible ap_bringup_tid thread
	 * (must NOT be the cooperative system workqueue — see ROOT-CAUSE NOTE). */
	LOG_INF("WiFi AP glue ready; AP coming up in %d s on a preemptible thread",
		AP_START_DELAY_MS / 1000);
#else
	LOG_INF("WiFi AP glue ready; auto-start OFF — bring up manually: "
		"wifi ap enable -s \"zpsu-test\" -k 1 -p \"zpsu1234\" -c 6");
#endif
	return 0;
}

SYS_INIT(wifi_ap_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
