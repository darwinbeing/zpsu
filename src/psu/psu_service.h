#ifndef PSU_PSU_SERVICE_H_
#define PSU_PSU_SERVICE_H_

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

int psu_service_init(void);   /* SYS_INIT entry */
void psu_ui_init(void);       /* initial label sync */

/* LVGL event callbacks (registered from the SquareLine-generated UI). */
void psu_power_toggle_event_cb(lv_event_t* e);
void psu_mode_toggle_event_cb(lv_event_t* e);
void psu_current_set_event_cb(lv_event_t* e);

int psu_set_fan_rpm(int rpm);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSU_PSU_SERVICE_H_ */
