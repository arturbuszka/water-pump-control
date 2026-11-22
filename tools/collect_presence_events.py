#!/usr/bin/env python3
"""Polls GET /api/presence/events and appends finished PIR pulses to a JSONL file.

The board keeps only the last PRESENCE_EVENT_LOG_LEN pulses in RAM and has no
wall clock, so this turns each pulse into an absolute time and de-duplicates
by (boot, seq) across polls. One line per pulse:

    {"rise_at": "2026-10-18T14:03:12.400+02:00", "high_ms": 2600,
     "pump_on": false, "boot": "2026-10-18T09:12:00+02:00", "seq": 41}

Usage:
    tools/collect_presence_events.py http://waterpump.local presence_events.jsonl
    tools/collect_presence_events.py http://posejdon:8080/api/devices/fontanna \\
        presence_events.jsonl

The URL is the board itself, or a Posejdon device prefix; /api/presence/events
or /presence/events is appended accordingly. Stop with Ctrl+C.
"""
import argparse
import json
import sys
import time
import urllib.request
from datetime import datetime, timedelta


def events_url(base):
    base = base.rstrip("/")
    return base + ("/presence/events" if "/api/devices/" in base else "/api/presence/events")


def fetch(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.load(resp)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("base_url")
    ap.add_argument("out_file")
    ap.add_argument("--interval", type=float, default=60.0,
                    help="seconds between polls (default 60)")
    args = ap.parse_args()
    url = events_url(args.base_url)

    boot_at = None      # wall time of the board's boot, re-estimated per poll
    last_uptime = None
    written_seq = -1    # highest seq already in the file for this boot
    print(f"polling {url} every {args.interval:.0f} s -> {args.out_file}", file=sys.stderr)

    while True:
        try:
            data = fetch(url, timeout=10)
        except Exception as err:  # board offline, Wi-Fi blip, ...
            print(f"{datetime.now():%H:%M:%S} fetch failed: {err}", file=sys.stderr)
            time.sleep(args.interval)
            continue

        now = datetime.now().astimezone()
        uptime_ms = data["uptime_ms"]
        if last_uptime is None or uptime_ms < last_uptime:
            boot_at = now - timedelta(milliseconds=uptime_ms)
            written_seq = -1
            print(f"{now:%H:%M:%S} new boot at {boot_at:%Y-%m-%d %H:%M:%S}", file=sys.stderr)
        last_uptime = uptime_ms

        # Oldest first. Only the newest pulse can still be ongoing, so skipping
        # it leaves no hole - it is picked up on a later poll once it ends.
        fresh = sorted((e for e in data["events"]
                        if e["seq"] > written_seq and e["high_ms"] is not None),
                       key=lambda e: e["seq"])
        if fresh and fresh[0]["seq"] > written_seq + 1 and written_seq >= 0:
            print(f"{now:%H:%M:%S} lost pulses {written_seq + 1}..{fresh[0]['seq'] - 1}: "
                  f"poll more often", file=sys.stderr)
        with open(args.out_file, "a") as f:
            for e in fresh:
                rise_at = now - timedelta(milliseconds=e["rise_ago_ms"])
                f.write(json.dumps({
                    "rise_at": rise_at.isoformat(timespec="milliseconds"),
                    "high_ms": e["high_ms"],
                    "pump_on": e["pump_on"],
                    "boot": boot_at.isoformat(timespec="seconds"),
                    "seq": e["seq"],
                }) + "\n")
                written_seq = e["seq"]
        if fresh:
            print(f"{now:%H:%M:%S} +{len(fresh)} pulses (last seq {written_seq})", file=sys.stderr)
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
