#include "dfu.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <pico/bootrom.h>

LOG_MODULE_REGISTER(dfu, LOG_LEVEL_INF);

/*
 * Enter the RP2040 USB mask-ROM bootloader (BOOTSEL) directly via the bootrom
 * `reset_usb_boot()`. This is the reliable path the Pico SDK itself uses — it
 * does a clean full-chip entry into the USB bootloader, unlike sys_reboot()
 * (a bare Cortex-M SYSRESETREQ), which on RP2040 can leave USB unenumerated or
 * land in an undefined state. No retention boot-mode / reboot dance needed.
 *
 * Deferred ~200 ms so the trigger source can flush its reply (shell line / UDP
 * datagram / msgbox repaint) before USB drops.
 */
static void dfu_reboot_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("entering RP2040 USB BOOTSEL for firmware upgrade");
	reset_usb_boot(0, 0);   /* enters the mask-ROM USB bootloader; never returns */
}
static K_WORK_DELAYABLE_DEFINE(dfu_reboot_work, dfu_reboot_fn);

void dfu_enter_recovery(void)
{
	LOG_INF("DFU recovery requested; entering BOOTSEL in 200 ms");
	(void)k_work_schedule(&dfu_reboot_work, K_MSEC(200));
}

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Entering USB BOOTSEL for firmware upgrade (drag a .uf2)...");
	dfu_enter_recovery();
	return 0;
}
SHELL_CMD_REGISTER(dfu, NULL,
		   "Reboot into USB BOOTSEL for firmware upgrade",
		   cmd_dfu);
#endif /* CONFIG_SHELL */
