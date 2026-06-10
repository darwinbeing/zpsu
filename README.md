# HP HSTNS-PL30 DIY Raspberry Pi Pico Watt Meter

[//]: # (Image References)
[image1]: ./images/PL30PicoWattMeter.png "PICO Display Pack"
[image2]: ./images/PL30PicoWattMeter2.png "PICO Display Pack2"  
[image3]: ./images/pico_w.png "PICOW WIFI"  

![alt text][image1]
![alt text][image2]

### Build on Mac OS

1. Create an Anaconda virtual environment (Python >= 3.12, required by Zephyr v4.4.0)  
[Download Anaconda for Mac(Intel)](https://repo.anaconda.com/archive/Anaconda3-2024.02-1-MacOSX-x86_64.pkg)  
conda create -n zephyr python=3.12 && conda activate zephyr  
2. Installing Dependencies  
brew install cmake dtc

2. Install West  
pip install west

3. Initialize Zephyr(Tag: v4.4.0)  
west init --mr v4.4.0 ~/zephyrproject  
cd ~/zephyrproject  
west update  
pip install -r ~/zephyrproject/zephyr/scripts/requirements.txt  

4. Setting Up the Toolchain  
[macOS (x86_64) hosted cross toolchains AArch32 13.2.Rel1](https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-darwin-x86_64-arm-none-eabi.tar.xz?rev=a3d8c87bb0af4c40b7d7e0e291f6541b&hash=10927356ACA904E1A0122794E036E8DDE7D8435D)  
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb  
export GNUARMEMB_TOOLCHAIN_PATH=~/zephyrproject/arm-gnu-toolchain-13.2.Rel1-darwin-x86_64-arm-none-eabi  

5. Clone repo  
git clone https://github.com/darwinbeing/zpsu_mon.git ~/  

6. Compile  
cd ~/zpsu_mon  
source ~/zephyrproject/zephyr/zephyr-env.sh

    Pico RP2040 Display Pack  
    west build -b rpi_pico -d build_lcd1 -- -DCONFIG_PICO_DISPLAY_PACK=y
   
    Pico RP2040 Display Pack2  
    west build -b rpi_pico -d build_lcd2 -- -DCONFIG_PICO_DISPLAY_PACK2=y

    Pico2 RP2350A Display Pack  
    west build -b rpi_pico2/rp2350a/m33 -d build_lcd3 -- -DCONFIG_PICO_DISPLAY_PACK=y

    Pico2 RP2350A Display Pack2  
    west build -b rpi_pico2/rp2350a/m33 -d build_lcd4 -- -DCONFIG_PICO_DISPLAY_PACK2=y    

    Pico W RP2040 WiFi (one-time first: cd ~/zephyrproject && west blobs fetch hal_infineon)  
    west build -b rpi_pico/rp2040/w -d build_wifi -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf

    Pico W RP2040 AP (mutually exclusive with wifi.conf; same one-time blob fetch as above)  
    west build -b rpi_pico/rp2040/w -d build_ap -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=ap.conf
    The AP (zpsu-<MAC4> / zpsu1234, 192.168.4.1) auto-starts at boot; PSU control over
    UDP :5000 — nc -u 192.168.4.1 5000 then STATUS/ON/OFF/MODE CC/CC 5.0/FAN, or
    python3 psu_ap_smoke.py

    SoftAP shell commands (over the USB-CDC console):
      wifi ap enable -s "<ssid>" -k 1 -p "<psk>" -c 6   start the AP (-k 1 = WPA2-PSK)
      wifi ap disable                                    stop the AP
      wifi ap status                                     AP state
      wifi ap stations                                   list connected clients
      apset <ssid> <psk>                                 persist new AP creds + re-enable
      printf 'SETAP <ssid> <psk>\n' | nc -u 192.168.4.1 5000   same, over UDP :5000

    Pico W RP2040 USB-DFU upgrade build (MCUboot serial recovery; opt-in --sysbuild)
    west build --sysbuild -b rpi_pico/rp2040/w/mcuboot -d build_dfu_wifi -- \
        -Dzpsu_mon_CONFIG_PICO_DISPLAY_PACK2=y \
        -Dzpsu_mon_EXTRA_CONF_FILE="wifi.conf;dfu.conf" \
        -Dzpsu_mon_EXTRA_DTC_OVERLAY_FILE="boards/rpi_pico/pico_display_pack2.overlay;boards/rpi_pico/dfu_singleslot.dtsi"
    First install: west flash -d build_dfu_wifi (debug probe; flashes both images) or
    BOOTSEL-drag mcuboot.uf2 + a picotool-converted signed-app uf2. Then upgrades are
    USB-only, no button:
      trigger  shell `dfu`  |  UDP `printf 'DFU\n' | nc -u 192.168.4.1 5000`  |  hold A+Y 2 s
      upload   mcumgr --conntype serial --connstring "dev=<port>,baud=115200" \
                 image upload build_dfu_wifi/zpsu_mon/zephyr/zephyr.signed.bin
      boot new mcumgr --conntype serial --connstring "dev=<port>,baud=115200" reset
    Single application slot (overwrite-in-place; the WiFi image can't fit dual-slot in
    2 MB), MCUboot dev signing key — production must swap in a private key. Design +
    partition map + RP2350 follow-on: docs/superpowers/specs/2026-06-08-usb-dfu-upgrade-design.md

    SoftAP fix (Zephyr v4.4.0): the Infineon AIROC/WHD 3.3.3 firmware rejects the
    "chanspec" iovar in AP mode with WHD_WLAN_BADCHAN (2020) on every 2.4 GHz channel
    (`whd_wifi_init_ap` -> `whd_wifi_set_chanspec`), so SoftAP bring-up failed on
    v4.4.0 (it worked on v4.2.1/WHD 3.3.2). Fixed by `patches/cyw43439_ap_set_channel.patch`:
    `whd_wifi_init_ap` now sets the channel via the WLC_SET_CHANNEL ioctl (plain
    channel number, exactly as the Pico SDK cyw43-driver SoftAP does) for the 43439,
    instead of the chanspec iovar. Apply for local builds (from this repo root):
    `git -C ~/zephyrproject/modules/hal/infineon apply "$PWD/patches/cyw43439_ap_set_channel.patch"`
    (verified on hardware: AP `zpsu-<MAC4>` broadcasts and clients associate).

### Runtime WiFi/AP credentials (persist in flash, no rebuild)

Credentials live in NVS (a flash `storage_partition`), so you set them once at
runtime instead of baking them into the image:

> **Required patch:** `patches/rp2040_flash_ramfunc.patch` (applied by CI; apply
> manually from this repo root: `git -C ~/zephyrproject/modules/hal/rpi_pico apply
> "$PWD/patches/rp2040_flash_ramfunc.patch"`). It fixes the real cause of Zephyr
> #68728: three `hardware_flash/flash.c` helpers (`flash_wait_ready`,
> `flash_enable_write`, `flash_put_cmd_addr`) are `static inline` without the RAM
> attribute, so when the compiler doesn't inline them they sit in XIP flash and are
> called while XIP is disabled → HardFault → double fault → **MCU lockup** the first
> time a credential is saved. The patch forces them into RAM
> (`__no_inline_not_in_flash_func`).


    STA (join your WiFi): over the USB-CDC shell, then reboot —
      wifi cred add -s "<ssid>" -k 1 -p "<psk>"          (-k 1 = WPA2-PSK)
      wifi cred list / wifi cred delete -s "<ssid>"       (manage stored networks)

    AP (rename/secure the SoftAP): while joined to the AP, over UDP :5000 —
      printf 'SETAP <ssid> <psk>\n' | nc -u 192.168.4.1 5000   (psk 8-63 chars)
      or over the USB-CDC shell:  apset <ssid> <psk>
    The AP re-enables with the new creds (it drops you — rejoin with them).

Optional build-time seed: a gitignored `src/net/wifi_creds.h` / `src/net/ap_creds.h`
is written to the store once on first boot when it is empty; runtime changes
override thereafter. With no seed and nothing stored, the STA waits for a
`wifi cred add` and the AP falls back to `zpsu-<MAC4>` / `zpsu1234`.

### Pico W (WiFi STA) boot log + gateway ping

USB-CDC console of the `build_wifi` image (board `rpi_pico/rp2040/w`, Zephyr v4.4.0,
SSID redacted): the AIROC CYW43439 starts, the STA associates, DHCPv4 leases an
address, then `net ping` to the gateway returns 5/5. (The log begins where the
USB-CDC console enumerates, ~4 s after power-on, so the early boot banner is not shown.)

    [4050] WLAN MAC Address : D8:3A:DD:FE:98:13
    [4056] WLAN Firmware    : wl0: Jun  5 2024 06:33:59 version 7.95.88 (cf1d613 CY) FWID 01-7b7cf51a
    [4062] WLAN CLM         : API: 12.2 Data: 9.10.39 Compiler: 1.29.4 ClmImport: 1.36.3 Creation: 2024-04-16 21:20:55
    [4064] WHD VERSION      : 3.3.3.26653 : WIFI5-v3.3.3 : GCC 13.2 : 2025-04-14 03:18:50 +0000
    [00:00:04.583,000] <wrn> udc_rpi: BUS RESET
    PWM-based RGB LED control
    [00:00:04.703,000] <wrn> display_control: Display regulator control not supported
    [00:00:07.717,000] <inf> wifi_net: auto-connecting to "<your-ssid>" ...
    [00:00:10.660,000] <inf> wifi_net: WiFi associated; starting DHCPv4
    [00:00:15.092,000] <inf> wifi_net: WiFi up: 192.168.1.191 gw 192.168.1.1
    uart:~$ kernel version
    Zephyr version 4.4.0
    uart:~$ net ping -c 5 192.168.1.1
    PING 192.168.1.1
    28 bytes from 192.168.1.1 to 192.168.1.191: icmp_seq=1 ttl=64 time=19 ms
    28 bytes from 192.168.1.1 to 192.168.1.191: icmp_seq=2 ttl=64 time=12 ms
    28 bytes from 192.168.1.1 to 192.168.1.191: icmp_seq=3 ttl=64 time=12 ms
    28 bytes from 192.168.1.1 to 192.168.1.191: icmp_seq=4 ttl=64 time=12 ms
    28 bytes from 192.168.1.1 to 192.168.1.191: icmp_seq=5 ttl=64 time=15 ms

