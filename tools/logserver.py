#!/usr/bin/env python3
"""Log server for Brewdeck debug builds: receives the screens' log lines (UDP, syslog) and writes them to
one file per screen and day, e.g. logs/brewdeck-p4-2026-11-24.log. Python 3, no packages needed.

    python3 tools/logserver.py [folder] [--port 5140] [--quiet]

Point a screen at it with GET http://<screen>.local/loghost?to=<this computer's address>, or LOG_HOST in
wifi_config.h. VictoriaLogs (tools/victorialogs) or rsyslog can do the same job (a UDP input on the port and a file per host).
"""
import argparse
import datetime
import os
import re
import socket
import sys

# RFC 5424 as the screens send it ("<14>1 - brewdeck-p4 brewdeck - - - text"), or the short "<14>host: text"
LINE = re.compile(rb"^<\d+>(?:1 \S+ (\S+) \S+ \S+ \S+ \S+ |([A-Za-z0-9_.-]{1,40}): ?)(.*)$", re.S)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder", nargs="?", default="logs")
    ap.add_argument("--port", type=int, default=5140)
    ap.add_argument("--quiet", action="store_true", help="do not echo lines")
    a = ap.parse_args()
    os.makedirs(a.folder, exist_ok=True)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", a.port))
    print(f"listening on UDP {a.port}, writing to {os.path.abspath(a.folder)}", flush=True)
    files = {}
    while True:
        data, (addr, _) = s.recvfrom(2048)
        m = LINE.match(data)
        name, text = ((m.group(1) or m.group(2)).decode(), m.group(3)) if m else (addr, data)
        text = text.decode("utf-8", "replace").rstrip("\r\n")
        day = datetime.date.today().isoformat()
        key = (name, day)
        f = files.get(key)
        if f is None:
            for k in [k for k in files if k[0] == name]:
                files.pop(k).close()
            f = files[key] = open(os.path.join(a.folder, f"{name}-{day}.log"), "a", encoding="utf-8")
        f.write(text + "\n")
        f.flush()
        if not a.quiet:
            print(f"{name}: {text}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
