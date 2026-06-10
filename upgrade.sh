#!/usr/bin/env bash
#
# Software-triggered USB firmware upgrade for the zpsu-mon Pico W (Path A).
#
# Triggers the running firmware into the RP2040 USB BOOTSEL bootloader (the app's
# `dfu` shell command -> bootrom reset_usb_boot()), then flashes a new image with
# picotool. No BOOTSEL button press needed.
#
# Usage:
#   ./upgrade.sh [path/to/firmware.uf2]      # default: build_dfu/zephyr/zephyr.uf2
#
# Requirements (macOS): picotool, python3 + pyserial.
#
# Notes:
#   - Build the new firmware WITH dfu.conf, e.g.
#       west build -b rpi_pico/rp2040/w -d build_dfu -- \
#           -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE="wifi.conf;dfu.conf"
#     otherwise the upgraded image won't have the `dfu` trigger for next time.
#   - After the flash-reboot the USB-CDC console may need one unplug/replug to
#     re-enumerate (RP2040 + macOS USB quirk); the upgrade itself always succeeds.
#   - If the device is already in BOOTSEL, this just flashes.
#
set -euo pipefail

UF2="${1:-build_dfu/zephyr/zephyr.uf2}"

if [[ ! -f "$UF2" ]]; then
	echo "ERROR: firmware not found: $UF2" >&2
	exit 1
fi

in_bootsel() { [[ -d /Volumes/RPI-RP2 ]]; }
app_port()   { ls /dev/cu.usbmodem* 2>/dev/null | head -1; }

# 1) Trigger into BOOTSEL (unless already there).
if ! in_bootsel; then
	PORT="$(app_port || true)"
	if [[ -z "$PORT" ]]; then
		echo "No app serial port found and device is not in BOOTSEL." >&2
		echo "Power-cycle the board (or hold BOOTSEL while plugging in), then re-run." >&2
		exit 1
	fi
	echo "Triggering DFU on $PORT ..."
	# The write drops USB as the device resets — ignore the resulting error.
	python3 -c "import serial,sys; serial.Serial(sys.argv[1],115200).write(b'dfu\r\n')" "$PORT" 2>/dev/null || true

	printf "Waiting for BOOTSEL"
	for _ in $(seq 1 40); do
		in_bootsel && break
		printf "."
		sleep 0.5
	done
	printf "\n"
	if ! in_bootsel; then
		echo "ERROR: device did not enter BOOTSEL (try a power-cycle)." >&2
		exit 1
	fi
fi

# 2) Flash the new image and run it.
echo "Flashing $UF2 ..."
picotool load -x "$UF2"

echo "Done — new firmware is running."
echo "(If the USB console doesn't reappear, unplug/replug the board once.)"
