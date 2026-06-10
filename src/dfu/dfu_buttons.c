/*
 * On-device USB-DFU trigger for builds with no shell/UDP (the non-WiFi watch
 * builds). Hold buttons A + Y together for 2 s -> a confirm msgbox -> "Upgrade"
 * -> dfu_enter_recovery(). The deliberate two-button long hold is the guard
 * against accidental entry (spec 5.2). Compiled only into the mcuboot upgrade
 * build (CMakeLists gate on CONFIG_BOOTLOADER_MCUBOOT).
 *
 * LVGL 9.x msgbox API. LVGL is not thread-safe; the project drives LVGL from
 * system-workqueue work items, so the msgbox is created from a k_work item
 * (the hold timer), not from the input callback context.
 */
#include "dfu.h"

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(dfu_buttons, LOG_LEVEL_INF);

#define DFU_HOLD_TIME K_SECONDS(2)

static bool a_down;          /* INPUT_KEY_1    (button A) */
static bool y_down;          /* INPUT_KEY_DOWN (button Y) */
static lv_obj_t *dfu_mbox;   /* the open confirm box, or NULL */

static void dfu_upgrade_cb(lv_event_t *e)
{
	lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);

	LOG_INF("DFU confirmed from on-screen msgbox");
	lv_msgbox_close(mbox);
	dfu_mbox = NULL;
	dfu_enter_recovery();
}

static void dfu_cancel_cb(lv_event_t *e)
{
	lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);

	lv_msgbox_close(mbox);
	dfu_mbox = NULL;
}

/* Runs on the system workqueue after a sustained A+Y hold. */
static void dfu_confirm_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!(a_down && y_down) || dfu_mbox != NULL) {
		return;   /* released during the hold, or a box is already open */
	}

	lv_obj_t *mbox = lv_msgbox_create(NULL);   /* NULL -> modal on the top layer */

	lv_msgbox_add_title(mbox, "Firmware Upgrade");
	lv_msgbox_add_text(mbox, "Reboot into USB recovery?");

	lv_obj_t *ok = lv_msgbox_add_footer_button(mbox, "Upgrade");
	lv_obj_t *cancel = lv_msgbox_add_footer_button(mbox, "Cancel");

	lv_obj_add_event_cb(ok, dfu_upgrade_cb, LV_EVENT_CLICKED, mbox);
	lv_obj_add_event_cb(cancel, dfu_cancel_cb, LV_EVENT_CLICKED, mbox);
	lv_obj_center(mbox);
	dfu_mbox = mbox;

	LOG_INF("A+Y held: DFU confirm shown");
}
static K_WORK_DELAYABLE_DEFINE(dfu_hold_work, dfu_confirm_fn);

static void dfu_keys_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == INPUT_KEY_1) {
		a_down = evt->value;
	} else if (evt->code == INPUT_KEY_DOWN) {
		y_down = evt->value;
	} else {
		return;
	}

	if (a_down && y_down) {
		(void)k_work_schedule(&dfu_hold_work, DFU_HOLD_TIME);
	} else {
		(void)k_work_cancel_delayable(&dfu_hold_work);
	}
}
INPUT_CALLBACK_DEFINE(NULL, dfu_keys_cb, NULL);
