#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <math.h>

#include "ui.h"
#include <display_control.h>
#include "watchface_app.h"
#include "psu_service.h"

// #define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
// LOG_MODULE_REGISTER(app);

LOG_MODULE_REGISTER(main, LOG_LEVEL_WRN);

#define RENDER_INTERVAL_LVGL    K_MSEC(100)

typedef enum ui_state {
        INIT_STATE,
        SETTINGS_STATE,
        WATCHFACE_STATE,
        APPLICATION_MANAGER_STATE,
        NOTIFCATION_STATE,
        NOTIFCATION_LIST_STATE,
} ui_state_t;

void run_init_work(struct k_work *item);
static int pwm_rgb_led_init(void);

static const struct pwm_dt_spec red_pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(red_pwm_led));
static const struct pwm_dt_spec green_pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(green_pwm_led));
static const struct pwm_dt_spec blue_pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(blue_pwm_led));
static uint32_t fanRPM = 3200;
static uint8_t last_brightness = 5;

static ui_state_t watch_state = INIT_STATE;

K_WORK_DEFINE(init_work, run_init_work);

#define CONFIG_BUTTON_PSUCTRL_THREAD_PRIO 8
#define CONFIG_BUTTON_PSUCTRL_STACK_SIZE 1024

// K_THREAD_DEFINE(psuCtrlThreadId, CONFIG_BUTTON_PSUCTRL_STACK_SIZE,
//                 psu_service_init, NULL, NULL, NULL, CONFIG_BUTTON_PSUCTRL_THREAD_PRIO, 0, K_TICKS_FOREVER);

void run_init_work(struct k_work *item)
{
        pwm_rgb_led_init();
        // psu_service_init() is registered via SYS_INIT() in psu_service.cpp
        // Not to self, PWM consumes like 250uA...
        // Need to disable also when screen is off.
        display_control_init();
        display_control_power_on(true);

	static const struct device *lvgl_btn_dev;
	lvgl_btn_dev = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_button_input));
	if (!device_is_ready(lvgl_btn_dev))
	{
		LOG_ERR("Device not ready, aborting...");
		return 0;
	}

        watch_state = WATCHFACE_STATE;
        watchface_app_start(NULL);
        // k_thread_start(psuCtrlThreadId);
}

static int pwm_rgb_led_init(void)
{
	uint32_t pulse_red = 0, pulse_green = 0, pulse_blue = 0; /* pulse widths */
	int ret;

	printk("PWM-based RGB LED control\n");

	if (!device_is_ready(red_pwm_led.dev) ||
	    !device_is_ready(green_pwm_led.dev) ||
	    !device_is_ready(blue_pwm_led.dev)) {
		printk("Error: one or more PWM devices not ready\n");
		return 0;
	}

        // uint32_t pulse_red = red_pwm_led.period, pulse_green = green_pwm_led.period, pulse_blue = blue_pwm_led.period; /* pulse widths */
        ret = pwm_set_pulse_dt(&red_pwm_led, pulse_red);
        if (ret != 0) {
                printk("Error %d: red write failed\n", ret);
                return 0;
        }

        ret = pwm_set_pulse_dt(&green_pwm_led, pulse_green);
        if (ret != 0) {
                printk("Error %d: green write failed\n", ret);
                return 0;
        }
        ret = pwm_set_pulse_dt(&blue_pwm_led, pulse_blue);
        if (ret != 0) {
                printk("Error %d: blue write failed\n", ret);
                return 0;
        }
	return 0;
}

int main(void)
{
        // The init code requires a bit of stack.
        // So in order to not increase CONFIG_MAIN_STACK_SIZE and loose
        // this RAM forever, instead re-use the system workqueue for init
        // it has the required amount of stack.
        k_work_submit(&init_work);

        return 0;
}
