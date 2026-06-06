#ifndef APP_WATCHFACE_APP_HOOKS_H_
#define APP_WATCHFACE_APP_HOOKS_H_

#include <stdint.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/bluetooth/conn.h>

#ifdef __cplusplus
extern "C" {
#endif

void watchface_app_on_ble_comm(const struct zbus_channel* chan);
void watchface_app_on_accel(const struct zbus_channel* chan);
void watchface_app_on_chg(const struct zbus_channel* chan);
void watchface_app_on_psu(const struct zbus_channel* chan);
void watchface_app_on_connected(struct bt_conn* conn, uint8_t err);
void watchface_app_on_disconnected(struct bt_conn* conn, uint8_t reason);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* APP_WATCHFACE_APP_HOOKS_H_ */
