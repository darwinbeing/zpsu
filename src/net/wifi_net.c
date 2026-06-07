/*
 * Pico W WiFi connectivity glue.
 *
 * Registers net_mgmt callbacks to (1) start DHCPv4 automatically once the
 * CYW43439 associates with an AP, and (2) log the acquired IPv4 address when
 * the lease binds. Compiled only when CONFIG_WIFI is set (the wifi.conf opt-in
 * fragment). Credentials are entered at runtime via `wifi connect`.
 *
 * Design: docs/superpowers/specs/2026-06-07-pico-w-wifi-design.md
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(wifi_net, LOG_LEVEL_INF);

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

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
		} else {
			LOG_INF("WiFi associated; starting DHCPv4");
			net_dhcpv4_start(iface);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		/* Release the lease so we don't keep a stale IP; a later
		 * reconnect re-runs DHCPv4 from the connect handler above. */
		LOG_INF("WiFi disconnected");
		net_dhcpv4_stop(iface);
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

	LOG_INF("WiFi glue ready; connect with: wifi connect \"<ssid>\" <psk>");
	return 0;
}

SYS_INIT(wifi_net_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
