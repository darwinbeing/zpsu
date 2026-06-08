#!/usr/bin/env python3
"""Capture the Pico W (WiFi STA) boot log over USB-CDC, then auto-ping the
gateway and capture the replies.

Resilient to the USB re-enumeration that a board reset causes: when the CDC
device drops, it re-globs /dev/cu.usbmodem* and reopens as fast as the OS lets
it. Writes the raw stream to the log file for pasting into the README.

Usage: capture_wifi_boot.py [glob] [logfile]
"""
import glob
import re
import sys
import time

import serial

PATTERN = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem*"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/wifi_boot.log"

HARD_TIMEOUT = 120.0     # absolute cap (s)
WIFI_UP_WAIT = 25.0      # if no "WiFi up" by now, fall back to `net iface`
PING_COUNT = 5

gw_re = re.compile(r"gw\s+(\d+\.\d+\.\d+\.\d+)")
iface_gw_re = re.compile(r"[Gg]ateway[^0-9]*(\d+\.\d+\.\d+\.\d+)")

start = time.time()
logf = open(LOG, "w", buffering=1)

ser = None
state = "boot"           # boot -> pinged -> done
gw = None
ping_sent_at = None
asked_iface = False
buf = ""

def open_port():
    devs = sorted(glob.glob(PATTERN))
    if not devs:
        return None
    try:
        s = serial.Serial(devs[0], 115200, timeout=0.1)
        print(f"[capture] opened {devs[0]}", flush=True)
        return s
    except (serial.SerialException, OSError):
        return None

def send(cmd):
    try:
        ser.write((cmd + "\r\n").encode())
        ser.flush()
        print(f"[capture] >>> {cmd}", flush=True)
    except (serial.SerialException, OSError):
        pass

print(f"[capture] watching {PATTERN}; reset the board now...", flush=True)

while True:
    now = time.time()
    if now - start > HARD_TIMEOUT:
        print("[capture] hard timeout", flush=True)
        break

    if ser is None:
        ser = open_port()
        if ser is None:
            time.sleep(0.05)
            continue

    try:
        data = ser.read(256).decode("utf-8", "replace")
    except (serial.SerialException, OSError):
        # board reset -> device went away; drop handle and re-find it
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        continue

    if data:
        logf.write(data)
        sys.stdout.write(data)
        sys.stdout.flush()
        buf += data
        lines = buf.split("\n")
        buf = lines[-1]
        for line in lines[:-1]:
            if state == "boot":
                m = gw_re.search(line)
                if m:
                    gw = m.group(1)
                    time.sleep(1.0)
                    send(f"net ping -c {PING_COUNT} {gw}")
                    state, ping_sent_at = "pinged", time.time()
                elif asked_iface:
                    m2 = iface_gw_re.search(line)
                    if m2 and m2.group(1) != "0.0.0.0":
                        gw = m2.group(1)
                        time.sleep(0.5)
                        send(f"net ping -c {PING_COUNT} {gw}")
                        state, ping_sent_at = "pinged", time.time()

    if state == "boot" and not asked_iface and now - start > WIFI_UP_WAIT:
        print("[capture] no 'WiFi up' seen; querying `net iface`", flush=True)
        send("net iface")
        asked_iface = True

    if state == "pinged" and time.time() - ping_sent_at > 12.0:
        state = "done"
        break

if ser is not None:
    ser.close()
logf.close()
print(f"\n[capture] saved -> {LOG} (gw={gw})", flush=True)
