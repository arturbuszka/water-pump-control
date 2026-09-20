# waterpumpmod

Networked controller for a 5V water pump / cat fountain, built on an **ESP32 DevKit
board with an ESP-WROOM-32 module** and a single-channel opto-isolated relay module
(10A/250VAC contacts, 5V coil).

Written against **ESP-IDF**. The board stays awake on WiFi and exposes a small REST
API, so it waters on a schedule *and* pours on demand — a cat that walks up between
cycles can be served from the phone instead of waiting for the next interval.

> **This build is mains powered, not battery powered.** An always-on radio and CPU
> draw roughly 50–80 mA. Earlier revisions deep-slept between cycles for a ~10 µA
> average; that is gone, because a sleeping board cannot answer the app. The pump
> needs a 5V supply anyway.

## Wiring

```
5V ──┬──────────────────────────── ESP32  VIN (5V) pin
     │
     ├──────────────────────────── RELAY  VCC
     │
     └── CUT the pump's + wire ──► RELAY  COM
                                   RELAY  NO ──► pump  +

GND ─┬──────────────────────────── ESP32  GND
     ├──────────────────────────── RELAY  GND
     └──────────────────────────── pump  −      (leave intact)

ESP32 GPIO27 ─────────────────────RELAY  IN
```

### Rules worth following

- **Cut only the pump's positive (+) wire.** Ground must stay continuous and shared
  between the ESP32, the relay and the pump. Without a common ground the IN signal
  has no reference and the relay behaves erratically.
- **Use NO, not NC.** With the contacts normally open, a power loss or MCU reset
  leaves the pump off. On NC it would be the reverse — a reset would mean flooding.
- The module is **active-LOW**: pulling IN low closes COM↔NO.

### Two things that may not work on the first try

**GPIO27 outputs 3.3V, the module runs on 5V.** Most opto-isolated modules trigger
fine at 3.3V, but not all of them. If the dry run produces no click, add a driver:

```
GPIO27 ─[ 1 kΩ ]── base   2N2222
                   emitter ── GND
                   collector ── relay IN
```
Note the transistor **inverts** the signal, so set `RELAY_ACTIVE_LOW` to `0` if you
add one.

**Brownout.** The pump and the board draw from the same supply. If starting the pump
resets the board, add a 470–1000 µF capacitor across the 5V rail, or power the pump
separately while keeping the ground shared.

## How it works

`app_main()` starts four things and returns; FreeRTOS keeps them running:

```
                  schedule timer ─┐
  HTTP POST /api/pump/on ─────────┼──► request queue ──► pump task ──► relay
       (future) LD2410C radar ────┘
```

Everything that can ask for water puts a request on one queue, and a single **pump
task** ([src/pump.c](src/pump.c)) is the only code that touches the GPIO. That is
what keeps a manual pour and a scheduled cycle from fighting over the relay — they
queue instead of racing.

Details worth knowing:

- **Watering does not depend on the network.** The pump task and its timer are
  started before WiFi and never wait on it. If the router disappears the fountain
  keeps running its schedule; only the API goes quiet. The WiFi layer retries with
  a backoff up to 60 s, forever.
- **A run can be cut short.** The pump task waits in one-second slices on a task
  notification rather than a flat delay, so `POST /api/pump/off` stops the pump
  within about a second instead of at the end of the run.
- **A stop only cancels the run it was aimed at.** Each run carries an id, so a stop
  that arrives just as one run ends cannot cancel the next one.
- **The safety cap is enforced at runtime**, on every request, whatever its source —
  see `PUMP_MAX_ON_SECONDS` below.

## REST API

The board announces itself over mDNS as **`waterpump.local`**. If mDNS does not
resolve on your network (it is unreliable on some Windows setups and on guest VLANs),
use the IP address from the boot log instead. There is **no authentication** — anyone
on the LAN can run the pump.

| Method | Path | Body | Notes |
|---|---|---|---|
| `GET` | `/api/status` | — | State, counters and current config |
| `POST` | `/api/pump/on` | `{"seconds": 15}` | `seconds` optional → `manual_run_seconds` |
| `POST` | `/api/pump/off` | — | Cuts a running cycle short; no-op when idle |
| `GET` | `/api/config` | — | Current settings |
| `POST` | `/api/config` | any subset of fields | Persists to NVS, re-arms the schedule |

`GET /api/status` returns:

```json
{
  "pumping": false, "seconds_left": 0, "seconds_total": 0,
  "cycle_count": 12, "uptime_s": 43201, "wifi_rssi": -58,
  "mode": "interval", "interval_seconds": 3600,
  "run_seconds": 1200, "manual_run_seconds": 30
}
```

`POST /api/config` accepts `interval_seconds`, `run_seconds`, `manual_run_seconds`
and `mode` (`interval`, `on_demand`, `presence`). Omitted fields keep their value.
**Out-of-range values are rejected with HTTP 400, never silently clamped** — a typo
in the app surfaces as an error rather than as a surprise at the bowl. The rules:
run times must be 1…`PUMP_MAX_ON_SECONDS`, the interval at least 60 s, and a cycle
must be shorter than its own interval.

```sh
curl http://waterpump.local/api/status
curl -X POST http://waterpump.local/api/pump/on -d '{"seconds":10}'
curl -X POST http://waterpump.local/api/pump/off
curl -X POST http://waterpump.local/api/config -d '{"interval_seconds":7200}'
```

## Configuration

Live settings are stored in **NVS** and changed over the API. Reflashing does not
reset them; `pio run -t erase` does.

[include/config.h](include/config.h) holds the wiring constants and the factory
defaults used the first time the board boots with an empty NVS partition:

| Constant | Default | Meaning |
|---|---|---|
| `RELAY_GPIO` | `27` | GPIO driving the relay's IN pin |
| `RELAY_ACTIVE_LOW` | `1` | A LOW level energises the relay |
| `PUMP_ON_SECONDS` | `1200` | Run time of one scheduled cycle (20 min) |
| `PUMP_INTERVAL_SECONDS` | `3600` | Gap between cycles (1 h) |
| `PUMP_MANUAL_ON_SECONDS` | `30` | Default duration of a "pour now" request |
| `RUN_ON_FIRST_BOOT` | `1` | Water on cold boot, or wait one interval |
| `PUMP_MAX_ON_SECONDS` | `1800` | Hard cap, enforced on every request (30 min) |

WiFi credentials go in `include/wifi_secrets.h`, which is gitignored. Copy the
template and fill it in:

```sh
cp include/wifi_secrets.h.example include/wifi_secrets.h
```

Board-level settings (flash size, UART console, CPU frequency) are in
[sdkconfig.defaults](sdkconfig.defaults), and the flash layout in
[partitions.csv](partitions.csv) — the default single-app table caps the application
at 1 MB, which this build nearly fills once WiFi, HTTP and mDNS are linked in.

## Operating modes

`mode` in the config selects where scheduled watering comes from:

- **`interval`** — pour every `interval_seconds`. The normal setting.
- **`on_demand`** — never pour by itself; only the app triggers the pump.
- **`presence`** — reserved for an **HLK-LD2410C** 24 GHz presence radar, which is
  not wired up yet. The firmware accepts and reports the mode, but currently behaves
  like `on_demand`. When the module arrives, its UART driver posts to the same
  request queue and nothing else has to change.

## Building

```sh
pio run                  # compile
pio run -t upload        # flash
pio device monitor       # logs, 115200
pio run -t erase         # wipe flash, including stored settings
pio run -t menuconfig    # ESP-IDF configuration UI
```

The mDNS component is pulled from the ESP Component Registry on the first build
(see [src/idf_component.yml](src/idf_component.yml)), so that build needs network
access.

Most DevKit boards auto-reset into the bootloader via DTR/RTS. If yours doesn't: hold
**BOOT (IO0)**, tap **EN/RST**, release BOOT.

## Dry run — do this before connecting the pump

1. Connect **only** the ESP32 and the relay (VCC / GND / IN). Leave COM and NO empty.
2. Fill in `include/wifi_secrets.h`, then flash and open the monitor.
3. Expected in the log: the banner, `WiFi started`, an IP address, `mDNS up`, and
   `REST API listening on port 80`. With `RUN_ON_FIRST_BOOT 1` you also get an
   audible **click**, the module's LED, and a click back.
   - No clicks → 3.3V isn't enough, add the transistor driver above.
   - Clicks inverted relative to the log → flip `RELAY_ACTIVE_LOW`.
4. Shorten the schedule for testing:
   `curl -X POST http://waterpump.local/api/config -d '{"interval_seconds":60,"run_seconds":3}'`
   You should hear a cycle every minute.
5. Check the manual path: `curl -X POST http://waterpump.local/api/pump/on -d '{"seconds":5}'`
   clicks immediately, and `/api/pump/off` during a run stops it within a second.
6. Check the guard rail: `curl -X POST http://waterpump.local/api/config -d '{"run_seconds":9999}'`
   must return **400**. If it returns 200, stop and fix that before adding water.
7. Put a multimeter in continuity mode across **COM–NO**: open when off, closed when
   on. This confirms you're using the right contact pair.
8. Pull the router or disconnect the AP for a few minutes. The schedule must keep
   running, and the API must answer again once the network is back.
9. Only now wire in the pump and watch one full cycle.
10. Restore your real settings through `/api/config`.

## Note on timing

The interval is a plain periodic timer measured from boot, not a time of day, and it
restarts from zero after a power cut. Drift is a few seconds per hour — irrelevant
for watering. If you ever need "every day at 08:00", that means SNTP (the board is on
WiFi now, so this is no longer far off) or an external RTC.
