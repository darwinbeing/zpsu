#ifndef WATCHFACE_ANIM_H
#define WATCHFACE_ANIM_H

#include <lvgl.h>
#include <events/psuctrl_event.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Real-time monitoring animations drawn on the bottom half of the watchface.
 *
 * This module is intentionally self-contained: it draws onto the existing
 * SquareLine screen (ui_Screen1) at runtime and never modifies the generated
 * ui_Screen1.c, so re-exporting the UI from SquareLine does not clobber it.
 */

/* Create the animated widgets (live power chart, load arc, connect spinner)
 * on the bottom half of `parent` (ui_Screen1). Call once, after ui_init(). */
void watchface_anim_init(lv_obj_t *parent);

/* Show the pure-vector boot splash on the top layer. It fills a loading ring,
 * then dissolves to reveal the watchface (~1.5s total) and deletes itself. */
void watchface_splash_show(void);

/* Feed one live PSU sample into the animations. Call at the end of
 * watchface_set_ep() so it runs in the same context as the label updates. */
void watchface_anim_update(const struct psuctrl_data_event *evt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WATCHFACE_ANIM_H */
