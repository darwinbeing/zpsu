# HP HSTNS-PL30 DIY Raspberry Pi Pico Watt Meter

[//]: # (Image References)
[image1]: ./images/PL30PicoWattMeter.png "PICO Display Pack"
[image2]: ./images/PL30PicoWattMeter2.png "PICO Display Pack2"  
[image3]: ./images/pico_w.png "PICOW WIFI"  

![alt text][image1]
![alt text][image2]

### Build on Mac OS

1. Create an Anaconda virtual environment  
[Download Anaconda for Mac(Intel)](https://repo.anaconda.com/archive/Anaconda3-2024.02-1-MacOSX-x86_64.pkg)  
2. Installing Dependencies  
brew install cmake dtc

2. Install West  
pip install west

3. Initialize Zephyr(Tag: v4.2.1)  
west init --mr v4.2.1 ~/zephyrproject  
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

### Raspberry Pi Pico W (RP2040) — WiFi

![alt text][image3]

WiFi support for the Pico W (Infineon CYW43439) is opt-in via the `wifi.conf`
fragment on the `rpi_pico/rp2040/w` board target: associate with an AP, get a
DHCP lease, and optionally auto-connect at boot. Default `rpi_pico` /
`rpi_pico2` builds are unaffected.

1. One-time, fetch the CYW43439 firmware blobs:

    cd ~/zephyrproject  
    west blobs fetch hal_infineon

2. (Optional) Enable boot auto-connect by creating a local, gitignored
credentials header from the template:

    cp src/net/wifi_creds.h.sample src/net/wifi_creds.h
    # edit src/net/wifi_creds.h with your SSID / password

3. Build the Display Pack2 image with WiFi:

    cd ~/zpsu_mon  
    west build -b rpi_pico/rp2040/w -d build_wifi -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf

The shell **and** logs are on the USB-CDC serial port (no J-Link needed); open
it with e.g. `screen /dev/tty.usbmodem* 115200`.

- **With `wifi_creds.h` present:** the device connects automatically a few
seconds after boot (retrying the WHD first-join), runs DHCPv4, and logs
`WiFi up: <ip> gw <gw>`. It also reconnects if the link drops.
- **Without it, or to connect manually:**

    uart:~$ wifi connect -s "<ssid>" -k 1 -p <psk>

  `-k` is the key-management type: `1` = WPA2-PSK, `5` = WPA3-SAE, `0` = open.

Check state with `net iface` (shows the IPv4 address) and `net ping <host>`.
Note: `wifi status` is **not** implemented by the upstream airoc driver — use
`net iface` instead.

Notes:
- WiFi uses PIO0, so it cannot be combined with the opt-in PIO+DMA display
driver (`CONFIG_DISPLAY_SPI_PIO_DMA`); the WiFi build uses the default PL022
display path.
- The RP2040 is near its RAM ceiling with WiFi + the full UI, so `wifi.conf`
single-buffers LVGL and trims the object pool to fit (~89% of SRAM). The
RP2350 / Pico 2 W (520 KB) would have more headroom.

