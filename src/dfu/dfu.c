#include "dfu.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(dfu, LOG_LEVEL_INF);

/* Deferred so the trigger source can flush its reply before the reboot. */
static void dfu_reboot_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

	if (rc != 0) {
		LOG_ERR("bootmode_set failed (%d); rebooting anyway", rc);
	}
	LOG_INF("rebooting into MCUboot USB serial recovery");
	sys_reboot(SYS_REBOOT_WARM);
}
static K_WORK_DELAYABLE_DEFINE(dfu_reboot_work, dfu_reboot_fn);

void dfu_enter_recovery(void)
{
	LOG_INF("DFU recovery requested; rebooting in 200 ms");
	(void)k_work_schedule(&dfu_reboot_work, K_MSEC(200));
}

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Entering MCUboot USB serial recovery (rebooting)...");
	dfu_enter_recovery();
	return 0;
}
SHELL_CMD_REGISTER(dfu, NULL,
		   "Reboot into MCUboot USB serial recovery for firmware upgrade",
		   cmd_dfu);
#endif /* CONFIG_SHELL */
