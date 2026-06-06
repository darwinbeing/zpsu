#include <zephyr/zbus/zbus.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include "watchface_app_hooks.h"

ZBUS_CHAN_DECLARE(ble_comm_data_chan);
ZBUS_LISTENER_DEFINE(watchface_ble_comm_lis, watchface_app_on_ble_comm);

ZBUS_CHAN_DECLARE(accel_data_chan);
ZBUS_LISTENER_DEFINE(watchface_accel_lis, watchface_app_on_accel);

ZBUS_CHAN_DECLARE(chg_state_data_chan);
ZBUS_LISTENER_DEFINE(watchface_chg_event, watchface_app_on_chg);

ZBUS_CHAN_DECLARE(psuctrl_data_chan);
ZBUS_LISTENER_DEFINE(watchface_psuctrl_event, watchface_app_on_psu);

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = watchface_app_on_connected,
    .disconnected = watchface_app_on_disconnected,
};
