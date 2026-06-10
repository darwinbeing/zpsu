/*
 * USB-DFU upgrade trigger. The single entry point every trigger source
 * (shell `dfu`, UDP `DFU`, the on-device button hold) calls to reboot the
 * device into MCUboot serial recovery. Compiled only into the mcuboot
 * upgrade build (gated in CMakeLists by CONFIG_BOOTLOADER_MCUBOOT).
 *
 * Design: docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md
 */
#ifndef DFU_DFU_H_
#define DFU_DFU_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Request a reboot into MCUboot USB serial recovery. Non-blocking: schedules a
 * short (200 ms) delayed work that sets the retention boot-mode flag and warm-
 * reboots, so the caller's reply (UDP datagram / shell line / msgbox repaint)
 * is flushed first. Safe to call from any thread/work context.
 */
void dfu_enter_recovery(void);

#ifdef __cplusplus
}
#endif

#endif /* DFU_DFU_H_ */
