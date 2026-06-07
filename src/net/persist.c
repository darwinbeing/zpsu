/*
 * Persistent-storage bring-up for the Pico W WiFi/AP builds.
 *
 * Initializes the Settings subsystem and loads stored keys (WiFi credentials
 * and AP config) from NVS at boot — before the deferred WiFi auto-connect / AP
 * bring-up read them. Never blocks boot: on failure the WiFi modules fall back
 * to their defaults. Compiled only when CONFIG_SETTINGS is set (wifi/ap conf).
 */
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(persist, LOG_LEVEL_INF);

/* Run early in the APPLICATION level, before the WiFi modules
 * (CONFIG_APPLICATION_INIT_PRIORITY == 90). */
#define PERSIST_INIT_PRIORITY 10

static int persist_init(void)
{
	int ret = settings_subsys_init();

	if (ret) {
		LOG_ERR("settings_subsys_init failed (%d); using defaults", ret);
		return 0;
	}
	ret = settings_load();
	if (ret) {
		LOG_ERR("settings_load failed (%d); using defaults", ret);
		return 0;
	}
	LOG_INF("settings loaded from NVS");
	return 0;
}

SYS_INIT(persist_init, APPLICATION, PERSIST_INIT_PRIORITY);
