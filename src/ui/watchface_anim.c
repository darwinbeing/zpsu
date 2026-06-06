#include <lvgl.h>
#include <string.h>

#include "watchface_anim.h"
#include "ui.h"

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

/* Which live value drives the scrolling chart. Defaults to power (this is a
 * watt meter); flip to VOLTS/AMPS for debugging before amps/watts read-back
 * is wired up in send_psuctrl_data_event(). */
#define ANIM_SRC_WATTS 0
#define ANIM_SRC_VOLTS 1
#define ANIM_SRC_AMPS  2
#ifndef ANIM_CHART_SRC
#define ANIM_CHART_SRC ANIM_SRC_WATTS
#endif

#define ANIM_CHART_POINTS 100      /* 100 pts x 500ms ~= 50s of history */
#define ANIM_VAL_SCALE    10       /* store value*10 for 0.1 resolution  */

#ifndef ANIM_LOAD_MAX_W
#define ANIM_LOAD_MAX_W   1200.0f  /* DPS1200: ~1200W at full load        */
#endif

/* Colours (RGB hex; LVGL handles the 16-bit swap from prj.conf) */
#define COL_POWER    0x00FF00      /* matches the power label             */
#define COL_CHART_BG 0x0A0A0A
#define COL_BORDER   0x303030
#define COL_DIV      0x202020
#define COL_ARC_TRK  0x303030
#define COL_ARC_IND  0xFF9500
#define COL_SPIN_IND 0x00AAFF

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static lv_obj_t          *s_chart;
static lv_obj_t          *s_arc;
static lv_obj_t          *s_spinner;
static lv_obj_t          *s_splash;
static lv_chart_series_t *s_ser;

static int32_t s_chart_max;     /* running peak for Y auto-scale (scaled) */
static int32_t s_arc_val;       /* last arc value, to animate from        */

static float s_last_v, s_last_a, s_last_w, s_last_e;
static bool  s_have_last;
static bool  s_cc_active;

/* ------------------------------------------------------------------------- */
/* Animation exec callbacks (signature: void(void *var, int32_t value))      */
/* ------------------------------------------------------------------------- */

static void anim_opa_cb(void *obj, int32_t v)
{
        lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void anim_translate_y_cb(void *obj, int32_t v)
{
        lv_obj_set_style_translate_y((lv_obj_t *)obj, v, LV_PART_MAIN);
}

static void anim_arc_value_cb(void *obj, int32_t v)
{
        lv_arc_set_value((lv_obj_t *)obj, v);
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Smoothly drive an int32 property from `from` to `to`. Cancels any in-flight
 * animation on the same (var, cb) pair first so values never fight. */
static void anim_to_i32(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                        int32_t from, int32_t to, uint32_t ms)
{
        lv_anim_delete(obj, cb);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, cb);
        lv_anim_set_values(&a, from, to);
        lv_anim_set_duration(&a, ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
}

/* Brief "tick" bob when a reading changes by more than `eps`. */
static void bob_on_change(lv_obj_t *label, float now, float prev, float eps)
{
        float d = now - prev;

        if (!label) {
                return;
        }
        if (d < 0) {
                d = -d;
        }
        if (d <= eps) {
                return;
        }

        lv_anim_delete(label, anim_translate_y_cb);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, label);
        lv_anim_set_exec_cb(&a, anim_translate_y_cb);
        lv_anim_set_values(&a, 0, -5);
        lv_anim_set_duration(&a, 90);
        lv_anim_set_reverse_duration(&a, 110);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
}

static bool label_text_is(lv_obj_t *label, const char *s)
{
        if (!label) {
                return false;
        }

        const char *t = lv_label_get_text(label);

        return t && strcmp(t, s) == 0;
}

/* Start/stop an infinite breathing pulse on the current reading while in CC. */
static void set_cc_pulse(bool cc)
{
        if (cc == s_cc_active || !ui_LabelCurrent) {
                s_cc_active = cc;
                return;
        }
        s_cc_active = cc;

        if (cc) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, ui_LabelCurrent);
                lv_anim_set_exec_cb(&a, anim_opa_cb);
                lv_anim_set_values(&a, 255, 90);
                lv_anim_set_duration(&a, 500);
                lv_anim_set_reverse_duration(&a, 500);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_start(&a);
        } else {
                lv_anim_delete(ui_LabelCurrent, anim_opa_cb);
                lv_obj_set_style_opa(ui_LabelCurrent, LV_OPA_COVER, LV_PART_MAIN);
        }
}

/* ------------------------------------------------------------------------- */
/* Boot splash — pure vector, drawn on the top layer, self-deletes           */
/* ------------------------------------------------------------------------- */

#ifndef SPLASH_TITLE
#define SPLASH_TITLE    "ZPSU"
#endif
#ifndef SPLASH_SUBTITLE
#define SPLASH_SUBTITLE "PSU MONITOR"
#endif

static void splash_done_cb(lv_anim_t *a)
{
        /* defer the delete so the anim engine is finished with this object */
        lv_obj_delete_async((lv_obj_t *)a->var);
        s_splash = NULL;
}

void watchface_splash_show(void)
{
        lv_display_t *disp = lv_display_get_default();
        int32_t w = lv_display_get_horizontal_resolution(disp);
        int32_t h = lv_display_get_vertical_resolution(disp);

        /* full-screen opaque overlay on the top layer (covers the watchface) */
        lv_obj_t *ov = lv_obj_create(lv_layer_top());
        s_splash = ov;
        lv_obj_set_size(ov, w, h);
        lv_obj_center(ov);
        lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(ov, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(ov, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ov, 0, LV_PART_MAIN);

        /* loading ring */
        lv_obj_t *ring = lv_arc_create(ov);
        lv_obj_set_size(ring, 140, 140);
        lv_obj_center(ring);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
        lv_arc_set_rotation(ring, 270);
        lv_arc_set_bg_angles(ring, 0, 360);
        lv_arc_set_range(ring, 0, 100);
        lv_arc_set_value(ring, 0);
        lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(ring, lv_color_hex(COL_DIV), LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ring, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);

        /* title + subtitle inside the ring */
        lv_obj_t *title = lv_label_create(ov);
        lv_label_set_text(title, SPLASH_TITLE);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -8);

        lv_obj_t *sub = lv_label_create(ov);
        lv_label_set_text(sub, SPLASH_SUBTITLE);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(sub, lv_color_hex(0x808080), LV_PART_MAIN);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 20);

        /* (1) fill the ring 0 -> 100 */
        lv_anim_t fill;
        lv_anim_init(&fill);
        lv_anim_set_var(&fill, ring);
        lv_anim_set_exec_cb(&fill, anim_arc_value_cb);
        lv_anim_set_values(&fill, 0, 100);
        lv_anim_set_duration(&fill, 900);
        lv_anim_set_delay(&fill, 100);
        lv_anim_set_path_cb(&fill, lv_anim_path_ease_out);
        lv_anim_start(&fill);

        /* (2) after the fill, dissolve the overlay to reveal the watchface */
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, ov);
        lv_anim_set_exec_cb(&fade, anim_opa_cb);
        lv_anim_set_values(&fade, 255, 0);
        lv_anim_set_duration(&fade, 400);
        lv_anim_set_delay(&fade, 1150);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_in_out);
        lv_anim_set_completed_cb(&fade, splash_done_cb);
        lv_anim_start(&fade);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void watchface_anim_init(lv_obj_t *parent)
{
        if (!parent) {
                return;
        }

        /* ---- live scrolling chart (bottom-left) ---- */
        s_chart = lv_chart_create(parent);
        lv_obj_set_size(s_chart, 224, 100);
        lv_obj_align(s_chart, LV_ALIGN_BOTTOM_LEFT, 2, -2);
        lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
        lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
        lv_chart_set_point_count(s_chart, ANIM_CHART_POINTS);
        lv_chart_set_div_line_count(s_chart, 3, 0);
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);

        lv_obj_set_style_bg_color(s_chart, lv_color_hex(COL_CHART_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_chart, lv_color_hex(COL_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_chart, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(s_chart, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_chart, 0, LV_PART_MAIN);
        lv_obj_set_style_line_color(s_chart, lv_color_hex(COL_DIV), LV_PART_MAIN);
        /* line only: hide the per-point markers */
        lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
        lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
        lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);

        s_ser = lv_chart_add_series(s_chart, lv_color_hex(COL_POWER),
                                    LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_values(s_chart, s_ser, LV_CHART_POINT_NONE);
        s_chart_max = 10;

        /* ---- load arc (bottom-right) ---- */
        s_arc = lv_arc_create(parent);
        lv_obj_set_size(s_arc, 84, 84);
        lv_obj_align(s_arc, LV_ALIGN_BOTTOM_RIGHT, -4, -8);
        lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
        lv_arc_set_rotation(s_arc, 135);
        lv_arc_set_bg_angles(s_arc, 0, 270);
        lv_arc_set_range(s_arc, 0, 100);
        lv_arc_set_value(s_arc, 0);
        s_arc_val = 0;
        lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ARC_TRK), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ARC_IND), LV_PART_INDICATOR);

        /* ---- connecting spinner (center, dropped on first sample) ---- */
        s_spinner = lv_spinner_create(parent);
        lv_obj_set_size(s_spinner, 48, 48);
        lv_obj_align(s_spinner, LV_ALIGN_BOTTOM_MID, 0, -44);
        lv_spinner_set_anim_params(s_spinner, 1000, 60);
        lv_obj_set_style_arc_color(s_spinner, lv_color_hex(COL_DIV), LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_spinner, lv_color_hex(COL_SPIN_IND),
                                   LV_PART_INDICATOR);

        s_have_last = false;
        s_cc_active = false;
        /* the boot intro is handled by watchface_splash_show() on the top layer */
}

void watchface_anim_update(const struct psuctrl_data_event *evt)
{
        if (!evt) {
                return;
        }

        /* first sample: drop the connecting spinner to reveal the chart */
        if (s_spinner) {
                lv_obj_delete(s_spinner);
                s_spinner = NULL;
        }

        /* ---- push a sample into the scrolling chart ---- */
#if ANIM_CHART_SRC == ANIM_SRC_VOLTS
        float src = evt->volts;
#elif ANIM_CHART_SRC == ANIM_SRC_AMPS
        float src = evt->amps;
#else
        float src = evt->watts;
#endif
        if (src < 0.0f) {
                src = 0.0f;
        }

        int32_t cv = (int32_t)(src * (float)ANIM_VAL_SCALE + 0.5f);

        if (cv > s_chart_max) {
                s_chart_max = cv;
                lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                                        s_chart_max + s_chart_max / 10 + 1);
        }
        if (s_chart && s_ser) {
                lv_chart_set_next_value(s_chart, s_ser, cv);
        }

        /* ---- load arc: power as % of full load, animated smoothly ---- */
        if (s_arc) {
                int32_t pct = (int32_t)(evt->watts / ANIM_LOAD_MAX_W * 100.0f + 0.5f);

                if (pct < 0) {
                        pct = 0;
                }
                if (pct > 100) {
                        pct = 100;
                }
                anim_to_i32(s_arc, anim_arc_value_cb, s_arc_val, pct, 400);
                s_arc_val = pct;
        }

        /* ---- value-change "tick": bob labels whose reading moved ---- */
        if (s_have_last) {
                bob_on_change(ui_LabelVoltage, evt->volts,  s_last_v, 0.05f);
                bob_on_change(ui_LabelCurrent, evt->amps,   s_last_a, 0.02f);
                bob_on_change(ui_LabelPower,   evt->watts,  s_last_w, 0.5f);
                bob_on_change(ui_LabelEnergy,  evt->energy, s_last_e, 0.005f);
        }
        s_last_v = evt->volts;
        s_last_a = evt->amps;
        s_last_w = evt->watts;
        s_last_e = evt->energy;
        s_have_last = true;

        /* ---- CC breathing pulse, driven by the CV/CC label text ----
         * No real CV/CC read-back exists yet (PSUCtrl_CVCC is a stub, the label
         * stays "CV"), so this stays dormant until the firmware sets "CC". */
        set_cc_pulse(label_text_is(ui_LabelCVCC, "CC"));
}
