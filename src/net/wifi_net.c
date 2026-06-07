/*
 * Pico W WiFi connectivity glue.
 *
 * - Registers net_mgmt callbacks: on WiFi associate, start DHCPv4; on lease
 *   bind, log the acquired IPv4 address; on link drop, release the lease.
 * - Optional boot-time auto-connect: if a gitignored src/net/wifi_creds.h is
 *   present (defining WIFI_AUTO_SSID / WIFI_AUTO_PSK), it issues the WiFi
 *   connect at startup and retries on the transient first-join failure
 *   (whd_wifi_join -EAGAIN) and on link drops. Without that header the device
 *   boots normally and you connect manually via `wifi connect` in the shell.
 *
 * Compiled only when CONFIG_WIFI is set (the wifi.conf opt-in fragment).
 * Design: docs/superpowers/specs/2026-06-07-pico-w-wifi-design.md
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>

/* Optional, gitignored credentials for boot-time auto-connect. */
#if defined(__has_include)
#  if __has_include("wifi_creds.h")
#    include "wifi_creds.h"
#  endif
#endif

LOG_MODULE_REGISTER(wifi_net, LOG_LEVEL_INF);

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

/* How long to wait before re-attempting a failed/dropped connection. */
#define WIFI_AUTO_RETRY_MS 5000

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

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

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status =
			(const struct wifi_status *)cb->info;

		if (status->status != 0) {
			LOG_WRN("WiFi connect failed (status %d)",
				status->status);
			wifi_auto_connect_retry();
		} else {
			LOG_INF("WiFi associated; starting DHCPv4");
			net_dhcpv4_start(iface);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		/* Release the lease so we don't keep a stale IP; auto-connect
		 * (if configured) re-associates and re-runs DHCPv4. */
		LOG_INF("WiFi disconnected");
		net_dhcpv4_stop(iface);
		wifi_auto_connect_retry();
		break;
	default:
		break;
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	char ip[NET_IPV4_ADDR_LEN];
	char gw[NET_IPV4_ADDR_LEN];

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *if_addr =
			&iface->config.ip.ipv4->unicast[i].ipv4;

		if (if_addr->addr_type != NET_ADDR_DHCP) {
			continue;
		}

		net_addr_ntop(AF_INET, &if_addr->address.in_addr,
			      ip, sizeof(ip));
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw,
			      gw, sizeof(gw));
		LOG_INF("WiFi up: %s gw %s", ip, gw);
		break;
	}
}

static int wifi_net_init(void)
{
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	LOG_INF("WiFi glue ready; connecting from stored credentials shortly "
		"(provision/override: wifi cred add -s <ssid> -k 1 -p <psk>)");
	/* Give the chip/iface a moment to settle before the first attempt. */
	k_work_reschedule(&wifi_auto_connect, K_SECONDS(3));
	return 0;
}

SYS_INIT(wifi_net_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
