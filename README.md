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
fragment and the `rpi_pico/rp2040/w` board target. It is connectivity-only:
associate with an AP, get a DHCP lease, and log the IP.

1. One-time, fetch the CYW43439 firmware blobs:

    cd ~/zephyrproject  
    west blobs fetch hal_infineon

2. Build the Display Pack2 image with WiFi:

    cd ~/zpsu_mon  
    west build -b rpi_pico/rp2040/w -d build_wifi -- -DCONFIG_PICO_DISPLAY_PACK2=y -DEXTRA_CONF_FILE=wifi.conf

3. Connect at runtime over the USB-CDC shell (credentials are not stored on
the device):

    uart:~$ wifi scan  
    uart:~$ wifi connect "<ssid>" <psk>

On association the device auto-runs DHCPv4 and logs the acquired IP over RTT
(`WiFi up: <ip> gw <gw>`). Verify with `net iface` / `net ping <host>`.

Notes:
- WiFi uses PIO0, so it cannot be combined with the opt-in PIO+DMA display
driver (`CONFIG_DISPLAY_SPI_PIO_DMA`); the WiFi build uses the default PL022
display path.
- The LVGL heap is trimmed for the WiFi build to fit RP2040 SRAM; the default
(non-W) builds are unaffected.

