#include "watchface_app.hpp"

#include "watchface_app_hooks.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <clock.h>
#include <battery.h>
#include <heart_rate_sensor.h>
#include <accelerometer.h>
#include <vibration_motor.h>
#include <ram_retention_storage.h>
#include <events/ble_data_event.h>
#include <events/accel_event.h>
#include <notification_manager.h>
#include <zephyr/zbus/zbus.h>
#include <zsw_charger.h>
#include <events/chg_event.h>
#include <events/psuctrl_event.h>
#include "watchface.hpp"
#include <display_control.h>   // for lvgl_update(); C++ forbids implicit declaration

LOG_MODULE_REGISTER(watcface_app, LOG_LEVEL_WRN);

#define WORK_STACK_SIZE 3000
#define WORK_PRIORITY   5
#define RENDER_INTERVAL_LVGL    K_MSEC(100)
#define ACCEL_INTERVAL          K_MSEC(100)
#define BATTERY_INTERVAL        K_MINUTES(1)
#define SEND_STATUS_INTERVAL    K_SECONDS(30)
#define DATE_UPDATE_INTERVAL    K_MINUTES(1)

namespace {

typedef enum work_type {
    UPDATE_CLOCK, OPEN_WATCHFACE, BATTERY, SEND_STATUS_UPDATE, PSU_STATUS_UPDATE, UPDATE_DATE
} work_type_t;

typedef struct delayed_work_item {
    struct k_work_delayable work;
    work_type_t             type;
} delayed_work_item_t;

void general_work(struct k_work* item);
void update_ui_from_event(struct k_work* item);
void check_notifications(void);
int read_battery(int* batt_mV, int* percent);

delayed_work_item_t battery_work = { .type = BATTERY };
delayed_work_item_t clock_work   = { .type = UPDATE_CLOCK };
delayed_work_item_t status_work  = { .type = SEND_STATUS_UPDATE };
delayed_work_item_t date_work    = { .type = UPDATE_DATE };
delayed_work_item_t psuctrl_work = { .type = PSU_STATUS_UPDATE };
delayed_work_item_t general_work_item;
struct k_work_sync canel_work_sync;
K_WORK_DEFINE(update_ui_work, update_ui_from_event);
ble_comm_cb_data_t last_data_update;

void general_work(struct k_work *item)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(item);
    delayed_work_item_t *the_work = CONTAINER_OF(dwork, delayed_work_item_t, work);
    switch (the_work->type) {
        case OPEN_WATCHFACE: {
            app::WatchfaceApp::Instance().set_running(true);
            ui::Watchface::Show();
            lvgl_update();
            /* __ASSERT(0 <= k_work_schedule(&battery_work.work, K_NO_WAIT), "FAIL battery_work"); */
            /* __ASSERT(0 <= k_work_schedule(&clock_work.work, K_NO_WAIT), "FAIL clock_work"); */
            /* __ASSERT(0 <= k_work_schedule(&date_work.work, K_SECONDS(1)), "FAIL clock_work"); */
            break;
        }
        case UPDATE_CLOCK: {
            struct tm *time = clock_get_time();
            LOG_INF("%d, %d, %d\n", time->tm_hour, time->tm_min, time->tm_sec);
            ui::Watchface::SetTime(time->tm_hour, time->tm_min, time->tm_sec);

            // TODO move from this file
            /* retained.current_time_seconds = clock_get_time_unix(); */
            /* retained_update(); */
            check_notifications();
            __ASSERT(0 <= k_work_schedule(&clock_work.work, K_SECONDS(1)), "FAIL clock_work");
            break;
        }
        case UPDATE_DATE: {
            struct tm *time = clock_get_time();
            ui::Watchface::SetDate(time->tm_wday, time->tm_mday);
            __ASSERT(0 <= k_work_schedule(&date_work.work, DATE_UPDATE_INTERVAL), "FAIL date_work");
        }
        case BATTERY: {
            int batt_mv;
            int batt_percent;
            static uint32_t count;

            if (read_battery(&batt_mv, &batt_percent) == 0) {
                ui::Watchface::SetBatteryPercent(batt_percent, batt_mv);
            }
            ui::Watchface::SetHrm(count % 220);
            //heart_rate_sensor_fetch(&hr_sample);
            count++;
            __ASSERT(0 <= k_work_schedule(&battery_work.work, BATTERY_INTERVAL),
                     "FAIL battery_work");
            break;
        }
        case SEND_STATUS_UPDATE: {
            // TODO move to main
            int batt_mv;
            int batt_percent;
            int msg_len;
            int is_charging;
            char buf[100];
            memset(buf, 0, sizeof(buf));

            /* if (read_battery(&batt_mv, &batt_percent) == 0) { */
            /*     is_charging = zsw_charger_is_charging(); */
            /*     msg_len = snprintf(buf, sizeof(buf), "{\"t\":\"status\", \"bat\": %d, \"volt\": %d, \"chg\": %d} \n", batt_percent, */
            /*                        batt_mv, is_charging); */
            /*     ble_comm_send(buf, msg_len); */
            /* } */
            __ASSERT(0 <= k_work_schedule(&status_work.work, SEND_STATUS_INTERVAL),
                     "Failed schedule status work");
            break;
        }
    }
}

/** A discharge curve specific to the power source. */
static const struct battery_level_point levels[] = {
    /*
    Battery supervisor cuts power at 3500mA so treat that as 0%
    TODO analyze more to get a better curve.
    */
    { 10000, 4150 },
    { 0, 3500 },
};

int read_battery(int *batt_mV, int *percent)
{
#if 0
    int rc = battery_measure_enable(true);
    if (rc != 0) {
        LOG_ERR("Failed initialize battery measurement: %d\n", rc);
        return -1;
    }
    // From https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/boards/nrf/battery/src/main.c
    *batt_mV = battery_sample();

    if (*batt_mV < 0) {
        LOG_ERR("Failed to read battery voltage: %d\n", *batt_mV);
        return -1;
    }

    unsigned int batt_pptt = battery_level_pptt(*batt_mV, levels);

    LOG_DBG("%d mV; %u pptt\n", *batt_mV, batt_pptt);
    *percent = batt_pptt / 100;

    rc = battery_measure_enable(false);
    if (rc != 0) {
        LOG_ERR("Failed disable battery measurement: %d\n", rc);
        return -1;
    }
#endif
    return 0;
}

void check_notifications(void)
{
    uint32_t num_unread = notification_manager_get_num();
    ui::Watchface::SetNumNotifications(num_unread);
}

void update_ui_from_event(struct k_work *item)
{
    if (app::WatchfaceApp::Instance().running()) {
        if (last_data_update.type == BLE_COMM_DATA_TYPE_WEATHER) {
            LOG_DBG("Weather: %s t: %d hum: %d code: %d wind: %d dir: %d", last_data_update.data.weather.report_text,
                    last_data_update.data.weather.temperature_c, last_data_update.data.weather.humidity, last_data_update.data.weather.weather_code,
                    last_data_update.data.weather.wind,
                    last_data_update.data.weather.wind_direction);
            ui::Watchface::SetWeather(last_data_update.data.weather.temperature_c, last_data_update.data.weather.weather_code);
        } else if (last_data_update.type == BLE_COMM_DATA_TYPE_SET_TIME) {
            k_work_reschedule(&date_work.work, K_SECONDS(1));
        }
        return;
    }
}

}  // namespace

namespace app {

WatchfaceApp& WatchfaceApp::Instance() {
  static WatchfaceApp instance;
  return instance;
}

void WatchfaceApp::Init() {
  k_work_init_delayable(&general_work_item.work, general_work);
  k_work_init_delayable(&battery_work.work, general_work);
  k_work_init_delayable(&clock_work.work, general_work);
  k_work_init_delayable(&status_work.work, general_work);
  k_work_init_delayable(&date_work.work, general_work);
  running_ = false;
}

void WatchfaceApp::Start(lv_group_t* group) {
  ARG_UNUSED(group);
  general_work_item.type = OPEN_WATCHFACE;
  __ASSERT(0 <= k_work_schedule(&general_work_item.work, K_NO_WAIT), "FAIL schedule");
}

void WatchfaceApp::Stop() {
  running_ = false;
  k_work_cancel_delayable_sync(&battery_work.work, &canel_work_sync);
  k_work_cancel_delayable_sync(&clock_work.work, &canel_work_sync);
  k_work_cancel_delayable_sync(&date_work.work, &canel_work_sync);
  ui::Watchface::Remove();
}

void WatchfaceApp::HandleBleComm(const struct zbus_channel* chan) {
  if (running_) {
    struct ble_data_event* event = (struct ble_data_event*)zbus_chan_msg(chan);
    memcpy(&last_data_update, &event->data, sizeof(ble_comm_cb_data_t));
    k_work_submit(&update_ui_work);
  }
}

void WatchfaceApp::HandleAccel(const struct zbus_channel* chan) {
  if (running_) {
    struct accel_event* event = (struct accel_event*)zbus_chan_msg(chan);
    if (event->data.type == ACCELEROMETER_EVT_TYPE_STEP) {
      ui::Watchface::SetStep(event->data.data.step.count);
    }
  }
}

void WatchfaceApp::HandleChg(const struct zbus_channel* chan) {
  if (running_) {
    struct chg_state_event* event = (struct chg_state_event*)zbus_chan_msg(chan);
    LOG_WRN("CHG: %d", event->is_charging);
    __ASSERT(0 <= k_work_reschedule(&status_work.work, K_MSEC(10)), "Failed schedule status work");
  }
}

void WatchfaceApp::HandlePsu(const struct zbus_channel* chan) {
  if (running_) {
    struct psuctrl_data_event* event = (struct psuctrl_data_event*)zbus_chan_msg(chan);
    ui::Watchface::SetEnergyPanel(event);
    lvgl_update();
  }
}

void WatchfaceApp::HandleConnected(struct bt_conn* conn, uint8_t err) {
  if (!running_) return;
  if (err) {
    LOG_ERR("Connection failed (err %u)", err);
    return;
  }
  __ASSERT(0 <= k_work_schedule(&status_work.work, K_MSEC(1000)), "FAIL status");
  ui::Watchface::SetBleConnected(true);
}

void WatchfaceApp::HandleDisconnected(struct bt_conn* conn, uint8_t reason) {
  if (!running_) return;
  ui::Watchface::SetBleConnected(false);
}

}  // namespace app

extern "C" {
void watchface_app_on_ble_comm(const struct zbus_channel* chan) { app::WatchfaceApp::Instance().HandleBleComm(chan); }
void watchface_app_on_accel(const struct zbus_channel* chan)    { app::WatchfaceApp::Instance().HandleAccel(chan); }
void watchface_app_on_chg(const struct zbus_channel* chan)      { app::WatchfaceApp::Instance().HandleChg(chan); }
void watchface_app_on_psu(const struct zbus_channel* chan)      { app::WatchfaceApp::Instance().HandlePsu(chan); }
void watchface_app_on_connected(struct bt_conn* conn, uint8_t err)       { app::WatchfaceApp::Instance().HandleConnected(conn, err); }
void watchface_app_on_disconnected(struct bt_conn* conn, uint8_t reason) { app::WatchfaceApp::Instance().HandleDisconnected(conn, reason); }
}  // extern "C"

static int watchface_app_init(void) {
  app::WatchfaceApp::Instance().Init();
  return 0;
}
SYS_INIT(watchface_app_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
