#include <zephyr/kernel.h>
#include <math.h>
#include <watchface_ui.h>
#include <lvgl.h>
#include <general_ui.h>
#include "ui.h"
#include "psu_service.h"
#include <zephyr/logging/log.h>


LOG_MODULE_REGISTER(watchface_ui, LOG_LEVEL_WRN);


#define SMALL_WATCHFACE_CENTER_OFFSET 38

#define USE_SECOND_HAND

const lv_img_dsc_t *get_icon_from_weather_code(int code);

void watchface_init(void)
{
}

void page_event_cb(lv_event_t *e)
{
}

void watchface_show(void)
{
	ui_init();
        psu_ui_init();
}

void watchface_remove(void)
{
}

void watchface_set_battery_percent(int32_t percent, int32_t value)
{
}

void watchface_set_hrm(int32_t value)
{
}

void watchface_set_step(int32_t value)
{
}

void watchface_set_time(int32_t hour, int32_t minute, int32_t second)
{
}

void watchface_set_num_notifcations(int32_t value)
{
}

void watchface_set_ble_connected(bool connected)
{
}

void watchface_set_weather(int8_t temperature, int weather_code)
{
}

void watchface_set_date(int day_of_week, int date)
{
}

const lv_img_dsc_t *get_icon_from_weather_code(int code)
{
}

#define max(a,b)                                \
        ({ __typeof__ (a) _a = (a);             \
                __typeof__ (b) _b = (b);        \
                _a > _b ? _a : _b; })

static int format_val(float num, char *buf) {
	if(num <= 0) {
		sprintf(buf, "%s", "00.00");
	} else if(0 < num && num < 1) {
		sprintf(buf, "%.3f", num);
	} else {
		sprintf(buf, "%.*f", max(0, 3 - (int)(floor(log10(fabs(num))))), num);
	}

	return 0;
}

void watchface_set_ep(struct psuctrl_data_event *evt)
{
        char buf[32];

        LOG_DBG("PSU: %f %f %f %f %d", evt->volts, evt->amps, evt->watts, evt->energy, evt->is_kWh);

        format_val(evt->volts, buf);
        lv_label_set_text(ui_LabelVoltage, buf);

        format_val(evt->amps, buf);
        lv_label_set_text(ui_LabelCurrent, buf);

        format_val(evt->watts, buf);
        lv_label_set_text(ui_LabelPower, buf);

        format_val(evt->energy, buf);
        if(evt->is_kWh) {
                lv_label_set_text(ui_LabelEnergy, buf);
                lv_label_set_text(ui_Label8, "kWh");
        } else {
                lv_label_set_text(ui_LabelEnergy, buf);
                lv_label_set_text(ui_Label8, "Wh");
        }
}
