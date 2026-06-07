#!/usr/bin/env python3
"""Smoke-test the Pico W AP UDP control protocol.

Join the device's AP (zpsu-XXXX), then run:  python3 psu_ap_smoke.py
Sends each command to 192.168.4.1:5000 and checks the reply shape.
"""
import socket
import sys

HOST, PORT = "192.168.4.1", 5000


def ask(sock, cmd, timeout=1.5):
    sock.settimeout(timeout)
    sock.sendto((cmd + "\n").encode(), (HOST, PORT))
    data, _ = sock.recvfrom(128)
    return data.decode().strip()


def main():
    checks = [
        ("STATUS", lambda r: r.startswith("V=") and "ON=" in r and "CC=" in r),
        ("ON", lambda r: r == "OK ON"),
        ("CC 5.0", lambda r: r == "OK CC 5.0"),
        ("MODE CC", lambda r: r == "OK MODE CC"),
        ("OFF", lambda r: r == "OK OFF"),
        ("BOGUS", lambda r: r.startswith("ERR")),
    ]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    failures = 0
    for cmd, ok in checks:
        try:
            reply = ask(sock, cmd)
        except socket.timeout:
            print(f"FAIL {cmd!r}: no reply (timeout)")
            failures += 1
            continue
        status = "PASS" if ok(reply) else "FAIL"
        if status == "FAIL":
            failures += 1
        print(f"{status} {cmd!r} -> {reply!r}")
    print(f"\n{len(checks) - failures}/{len(checks)} checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
