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
    The AP (zpsu-<MAC4> / zpsu1234, 192.168.4.1) auto-starts at boot; control over UDP :5000 — nc -u 192.168.4.1 5000 then STATUS/ON/OFF/MODE CC/CC 5.0/FAN, or python3 psu_ap_smoke.py

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

