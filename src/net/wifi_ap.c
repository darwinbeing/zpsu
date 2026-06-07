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

#ifndef AP_PSK
#define AP_PSK "zpsu1234"
#endif
#ifndef AP_CHANNEL
#define AP_CHANNEL 6
#endif

LOG_MODULE_REGISTER(wifi_ap, LOG_LEVEL_INF);

#define AP_RETRY_MS 5000

static struct net_mgmt_event_callback ap_cb;
static char ap_ssid[WIFI_SSID_MAX_LEN + 1];

static void ap_bringup_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ap_bringup, ap_bringup_fn);

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

	if (net_dhcpv4_server_start(iface, &pool_base) < 0) {
		LOG_WRN("DHCPv4 server failed to start (clients need static IP)");
	} else {
		LOG_INF("AP up: 192.168.4.1, DHCP pool from 192.168.4.2");
	}
}

static void ap_bringup_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = {0};
	int ret;

	if (iface == NULL) {
		k_work_reschedule(&ap_bringup, K_MSEC(AP_RETRY_MS));
		return;
	}

#if defined(AP_SSID)
	(void)snprintf(ap_ssid, sizeof(ap_ssid), "%s", AP_SSID);
#else
	struct net_linkaddr *ll = net_if_get_link_addr(iface);

	if (ll != NULL && ll->len >= 6) {
		(void)snprintf(ap_ssid, sizeof(ap_ssid), "zpsu-%02X%02X",
			       ll->addr[4], ll->addr[5]);
	} else {
		(void)snprintf(ap_ssid, sizeof(ap_ssid), "zpsu-0000");
	}
#endif

	params.ssid = (const uint8_t *)ap_ssid;
	params.ssid_length = strlen(ap_ssid);
	params.psk = (const uint8_t *)AP_PSK;
	params.psk_length = sizeof(AP_PSK) - 1;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = AP_CHANNEL;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("starting AP \"%s\" on ch %d ...", ap_ssid, AP_CHANNEL);
	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
	if (ret) {
		/* airoc first call can return -EAGAIN(-11); just retry. */
		LOG_WRN("AP enable failed (%d); retrying in %d ms", ret, AP_RETRY_MS);
		k_work_reschedule(&ap_bringup, K_MSEC(AP_RETRY_MS));
		return;
	}

	/* airoc_mgmt_ap_enable() does init_ap/start_ap synchronously and only
	 * returns 0 once the SoftAP is up; the AIROC driver does NOT raise
	 * NET_EVENT_WIFI_AP_ENABLE_RESULT (verified in airoc_wifi.c), so configure
	 * the static IP + DHCP server here rather than from the event handler. */
	ap_configure_ip(iface);
}

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

	LOG_INF("WiFi AP glue ready; bringing AP up shortly");
	/* Give the chip/iface a moment to settle (MAC must be read in). */
	k_work_reschedule(&ap_bringup, K_SECONDS(3));
	return 0;
}

SYS_INIT(wifi_ap_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
