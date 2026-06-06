#include <zephyr/zbus/zbus.h>
#include <events/psuctrl_event.h>

/* Channel + observer kept here (C) so the zbus macros compile cleanly.
 * Name is unchanged: it is part of the contract observed by watchface_app.c. */
ZBUS_CHAN_DEFINE(psuctrl_data_chan,
                 struct psuctrl_data_event,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(watchface_psuctrl_event),
                 ZBUS_MSG_INIT());
