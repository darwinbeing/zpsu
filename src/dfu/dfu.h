/*
 * Software-triggered USB firmware upgrade. The single entry point every trigger
 * source (shell `dfu`, UDP `DFU`, the on-device A+Y button hold) calls to drop
 * the device into the RP2040 USB BOOTSEL bootloader for a UF2/picotool upgrade.
 * Compiled only when CONFIG_APP_USB_DFU is set (opt-in via dfu.conf).
 *
 * Design: docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md
 */
#ifndef DFU_DFU_H_
#define DFU_DFU_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Enter the RP2040 USB BOOTSEL bootloader (via the bootrom reset_usb_boot()).
 * Non-blocking: schedules a short (~200 ms) delayed work so the caller's reply
 * (UDP datagram / shell line / msgbox repaint) is flushed before USB drops.
 * Safe to call from any thread/work context.
 */
void dfu_enter_recovery(void);

#ifdef __cplusplus
}
#endif

#endif /* DFU_DFU_H_ */
