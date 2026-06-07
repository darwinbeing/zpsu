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

/* Snapshot of PSU state for the UDP STATUS command. Live V/A/W/energy come
 * from the latest poll; output/mode/cc are read on demand. */
struct psu_status {
  float volts;
  float amps;
  float watts;
  float energy_wh;
  bool output_on;
  bool mode_cc;      /* true = constant-current, false = constant-voltage */
  float cc_amps;
};

/* Thread-safe command façade. All serialize high-level PsuController access
 * through one mutex (shared with the 500 ms poll and the LVGL UI callbacks).
 * Each returns 0 on success, -1 on I2C/checksum failure. */
int psu_cmd_toggle_output(bool *now_on);   /* flips output; *now_on = new state */
int psu_cmd_set_output(bool on);           /* sets output to a specific state   */
int psu_cmd_toggle_mode(bool *now_cc);     /* flips CV/CC; *now_cc = new state   */
int psu_cmd_set_mode(bool cc);             /* sets CV/CC                          */
int psu_cmd_set_current(float amps);       /* absolute constant-current setpoint */
int psu_cmd_set_fan(int rpm);
int psu_cmd_get_status(struct psu_status *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSU_PSU_SERVICE_H_ */
