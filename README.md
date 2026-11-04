# waterpumpmod

Networked controller for a 5V water pump / cat fountain, built on an **ESP32 DevKit
board with an ESP-WROOM-32 module** and a single-channel opto-isolated relay module
(10A/250VAC contacts, 5V coil).

Written against **ESP-IDF**. The board stays awake on WiFi and exposes a small REST
API, so it can pour on a schedule, on demand from the phone, or by itself when a
presence sensor (a PIR or an HLK-LD2410C radar) sees a cat at the bowl — for as long
as the cat stays, not for a fixed time. The phone app is a separate PWA, Posejdon,
which proxies this API.

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

### Adding the presence sensor (`presence` mode only)

Two sensors are supported; set `presence_sensor` over the API (`pir` or `ld2410`,
default `PRESENCE_SENSOR_DEFAULT`) to say which one is wired. It is read once at
boot, so a change takes effect after a restart (power-cycle the board or tap
**EN/RST**) — `GET /api/status` reports `restart_required: true` until then. Each has a small driver behind
[include/presence_sensor.h](include/presence_sensor.h)
([src/sensor_pir.c](src/sensor_pir.c), [src/sensor_ld2410.c](src/sensor_ld2410.c)).
Both put their HIGH/LOW "someone is there" signal on the same `OUT` pin.

**PIR motion sensor** (`pir`, e.g. HC-SR301 mini):

```
3.3V or 5V (check the module) ── PIR  VCC
GND ──────────────────────────── PIR  GND (shared with ESP32 and relay)
ESP32 GPIO33 ─────────────────── PIR  OUT
```

- The driver enables GPIO33's internal pull-down, so an unplugged or weakly
  driven sensor reads "nobody there".
- PIR modules settle for 30–60 s after power-up and can pulse meanwhile, so for
  `PIR_WARMUP_SECONDS` after boot `OUT` is logged and reported
  (`presence_warming_up: true`) but never triggers a pour.
- There is nothing to configure on the sensor, so the `presence_max_gate` /
  `_sensitivity` / `_no_one_seconds` fields are hidden from the API.

**HLK-LD2410C radar** (`ld2410`):

```
5V  ──────────────────────────── LD2410C  VCC
GND ──────────────────────────── LD2410C  GND (shared with ESP32 and relay)
ESP32 GPIO33 ───────────────────LD2410C  OUT
ESP32 GPIO17 (TX) ──────────────LD2410C  RX
ESP32 GPIO16 (RX) ──────────────LD2410C  TX
```

- **`OUT`** is what presence mode runs on, as with the PIR. Stock LD2410C firmware
  drives it active-HIGH; flip `PRESENCE_ACTIVE_HIGH` in `config.h` if a dry run shows
  the opposite.
- **`TX`/`RX`** are the sensor's own UART, wired to the ESP32's second UART port
  (distinct from UART0's console). The board only *writes* configuration through
  this link — sensitivity, distance gates, no-one timeout — at boot and whenever
  those settings change over the API; it doesn't read the sensor's richer
  distance/motion data back. See [src/ld2410_uart.c](src/ld2410_uart.c).
  `presence_sensor_responding` in the status says whether the last push was acked.

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

`app_main()` starts five things and returns; FreeRTOS keeps them running:

```
                  schedule timer ─┐
  HTTP POST /api/pump/on ─────────┼──► request queue ──► pump task ──► relay
     presence task (GPIO33) ──────┘

  boot, HTTP POST /api/config ──► sensor driver ──► LD2410C TX/RX (radar tuning only)
```

Everything that can ask for water puts a request on one queue, and a single **pump
task** ([src/pump.c](src/pump.c)) is the only code that touches the GPIO. That is
what keeps a manual pour and a scheduled cycle from fighting over the relay — they
queue instead of racing.

The **presence task** ([src/presence.c](src/presence.c)) samples `PRESENCE_GPIO`
every `PRESENCE_POLL_MS` in every mode — the reading always shows up in
`GET /api/status` as `presence_detected` and in the pulse log — but only acts on it
when `mode` is `presence`. How it decides is described in
[Presence mode](#presence-mode) below.

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

## Operating modes

`mode` in the config selects what makes the pump run:

- **`interval`** — pour `run_seconds` every `interval_seconds`. The schedule is
  always on in this mode.
- **`on_demand`** — pour when the app asks for it. Adds the schedule on top only if
  `schedule_enabled` is `true`.
- **`presence`** — pour when the presence sensor sees someone at the bowl, for as
  long as it keeps seeing them — see [Presence mode](#presence-mode). Adds the
  schedule on top only if `schedule_enabled` is `true`; a presence session never
  starts while a scheduled cycle is running.

`POST /api/pump/on` works in every mode. `schedule_enabled` is stored but ignored in
`interval` mode, so switching back to another mode restores whatever was set there.
`interval_seconds` is measured from the start of one cycle to the start of the next:
20 minutes of pumping followed by 60 minutes of rest is `run_seconds: 1200`,
`interval_seconds: 4800`.

## Presence mode

A sensor pulse says "something moved", not "a cat is drinking", so the board
does not react to single pulses. It keeps a **presence score** `x`: each pulse adds
1 (up to `PRESENCE_X_MAX`), and between pulses the score decays with time constant
`presence_hold_seconds`:

```
x[k] = min( x[k−1] · e^(−Δt / hold) + e[k],  x_max )
```

- **What counts as a pulse.** `e[k]` is 1 once `OUT` has been HIGH for
  `PRESENCE_MIN_PULSE_MS` (shorter spikes are noise), and again every
  `PRESENCE_REEMIT_MS` it stays HIGH — so a sensor that holds `OUT` HIGH through
  continuous motion keeps counting instead of looking like one pulse.
- **Start and stop.** The pump follows `x` with hysteresis: a session starts when `x`
  reaches the start threshold and ends when it falls below `PRESENCE_TH_OFF`. After
  the last pulse the pump keeps pouring for `hold · ln(x_peak / 0.3)` — some 20–30 s
  after a single PIR pulse (a cat walking past), up to ~40 s after a drinking
  session. The pour follows the visit instead of being fixed.
- **Ignoring a cat walking past.** With `presence_confirm_pulses: 2` the start
  threshold becomes `1 + e^(−window / hold)`, so a session starts only on a second
  pulse within `presence_confirm_window_seconds` — at the cost of a few seconds'
  delay.

Hard limits sit around the score:

| Limit | Setting | Why |
|---|---|---|
| Longest session | `presence_max_session_seconds` | A cat that never leaves, a sensor gone wrong |
| Rest between sessions | `presence_rest_seconds` | No rapid on/off of the relay |
| Re-arm only after quiet | — | A session cut short (max length, budget, the app's stop button) does not restart while the same cat is still there: the next one waits for the score to drop below the off threshold |
| Hourly budget | `presence_hourly_budget_seconds` | Hard cap on presence pouring per rolling hour |
| Blanking | `PRESENCE_BLANK_MS` | `OUT` is ignored right after the pump switches — relay spikes |
| Stuck sensor | `PRESENCE_STUCK_SECONDS` | `OUT` HIGH without a break is a fault (`presence_fault`), ignored until it drops |
| Other runs | — | A session never starts while another run holds the pump, and only ever stops its own run |

The decision lives in [src/presence_logic.c](src/presence_logic.c) (model described
in [include/presence_logic.h](include/presence_logic.h)), kept free of ESP-IDF so it
is unit-tested on a PC — see [Tests](#tests). The defaults come from a simulation of
cat visits; `presence_score` in the status shows `x` live, which is what to watch
when tuning `presence_hold_seconds`.

## REST API

The board announces itself over mDNS as **`waterpump.local`**. If mDNS does not
resolve on your network (it is unreliable on some Windows setups and on guest VLANs),
use the IP address from the boot log instead. There is **no authentication** — anyone
on the LAN can run the pump.

| Method | Path | Body | Notes |
|---|---|---|---|
| `GET` | `/api/status` | — | State, counters and current config |
| `GET` | `/api/history` | — | Last 10 finished runs |
| `GET` | `/api/presence/events` | — | Last 200 raw presence-sensor pulses |
| `POST` | `/api/pump/on` | `{"seconds": 15}` | `seconds` optional → `manual_run_seconds` |
| `POST` | `/api/pump/off` | — | Cuts a running cycle short; no-op when idle |
| `GET` | `/api/config` | — | Current settings |
| `POST` | `/api/config` | any subset of fields | Persists to NVS, re-arms the schedule |

### Status

`GET /api/status` returns the live state plus the current config:

```json
{
  "pumping": false, "seconds_left": 0, "seconds_total": 0,
  "cycle_count": 12, "uptime_s": 43201, "wifi_rssi": -58, "next_run_in_s": 1740,
  "presence_detected": false, "presence_sensor_active": "pir",
  "presence_sensor_responding": null, "presence_warming_up": false,
  "restart_required": false,
  "presence_score": 0.0, "presence_session": false, "presence_fault": false,
  "presence_budget_used_seconds": 0,
  "mode": "interval", "schedule_enabled": false, "interval_seconds": 3600,
  "run_seconds": 1200, "manual_run_seconds": 30,
  "presence_sensor": "pir", "presence_confirm_pulses": 1,
  "presence_confirm_window_seconds": 10, "presence_hold_seconds": 15,
  "presence_max_session_seconds": 300, "presence_rest_seconds": 10,
  "presence_hourly_budget_seconds": 1200
}
```

- `next_run_in_s` — time to the next scheduled cycle, or `null` when the schedule is
  not active (see [Operating modes](#operating-modes)). While the pump runs, the
  status also carries `source`: `schedule`, `manual` or `presence`.
- `presence_detected` — the sensor's live `OUT` reading, in every mode, so you can
  see the sensor work before switching into `presence` mode.
- `presence_sensor` is the configured sensor, `presence_sensor_active` the one this
  boot runs; they differ, and `restart_required` is `true`, after a change that is
  waiting for a restart.
- `presence_sensor_responding` — `null` for a sensor that cannot be checked (a PIR),
  otherwise whether the radar acked its last tuning push.
- `presence_warming_up` — the PIR is inside its post-boot warm-up and is not acted on.
- `presence_score` — the score `x` from [Presence mode](#presence-mode), two
  decimals; `presence_session` — a presence session is pouring; `presence_fault` —
  `OUT` is stuck HIGH and ignored; `presence_budget_used_seconds` — presence pouring
  in the last hour.

### History and pulse log

The board has no wall clock, so both give times relative to now and both live in
RAM — a reboot starts them over.

`GET /api/history` lists the last 10 finished runs, newest first:

```json
{"runs": [
  {"source": "schedule", "started_ago_s": 900, "seconds": 1200,
   "seconds_requested": 1200, "stopped_early": false}
]}
```

`GET /api/presence/events` lists the sensor's last 200 raw `OUT` pulses, newest first,
recorded in every mode at `PRESENCE_POLL_MS` resolution — the raw material for tuning
presence mode. `high_ms` is `null` while the pulse is still HIGH; `pump_on` says
whether the pump was running when it started, to spot pulses caused by the relay or
pump; `seq` restarts at 0 after a reboot.

```json
{"uptime_ms": 43201000, "events": [
  {"seq": 41, "rise_ago_ms": 5200, "high_ms": 2600, "pump_on": false}
]}
```

To keep more than 200 pulses, run
[tools/collect_presence_events.py](tools/collect_presence_events.py) on any machine
that can reach the board (or Posejdon). It polls the endpoint, converts times to wall
clock, and appends each finished pulse to a JSONL file:

```sh
tools/collect_presence_events.py http://waterpump.local presence_events.jsonl
```

### Changing the config

`POST /api/config` takes any subset of the fields below; omitted fields keep their
value. **Out-of-range values are rejected with HTTP 400, never silently clamped** — a
typo in the app surfaces as an error rather than as a surprise at the bowl. The
response carries the new config and `restart_required`.

| Field | Accepted | Notes |
|---|---|---|
| `mode` | `interval`, `on_demand`, `presence` | See [Operating modes](#operating-modes) |
| `schedule_enabled` | `true` / `false` | Ignored in `interval` mode |
| `interval_seconds` | ≥ 60 | Start to start |
| `run_seconds` | 1…`PUMP_MAX_ON_SECONDS`, < `interval_seconds` | One scheduled cycle |
| `manual_run_seconds` | 1…`PUMP_MAX_ON_SECONDS` | Default for `/api/pump/on` |
| `presence_sensor` | `pir`, `ld2410` | Takes effect after a restart |
| `presence_confirm_pulses` | 1 or 2 | Start on the first pulse, or on a second one |
| `presence_confirm_window_seconds` | 1…60 | Window for the second pulse |
| `presence_hold_seconds` | 1…300 | Decay time constant of the score |
| `presence_max_session_seconds` | 1…`PUMP_MAX_ON_SECONDS` | Longest single session |
| `presence_rest_seconds` | 0…3600 | Gap between sessions |
| `presence_hourly_budget_seconds` | 1…3600 | Presence pouring per rolling hour |
| `presence_max_gate` | 0…8 | Radar only, ~0.75 m per gate |
| `presence_sensitivity` | 0…100 | Radar only, 0 = most sensitive |
| `presence_no_one_seconds` | ≥ 1 | Radar only, its own "gone" timeout |

The three radar fields appear in the config and are accepted only while
`presence_sensor` is `ld2410` — checked against the sensor the request leaves
configured, so `{"presence_sensor":"ld2410","presence_max_gate":2}` works in one
call. With the radar running, a change to them is also pushed into the LD2410C over
its UART; if the sensor doesn't ack in time the call still succeeds (the setting is
saved), but a warning is logged and `presence_sensor_responding` turns `false`.

```sh
curl http://waterpump.local/api/status
curl http://waterpump.local/api/history
curl -X POST http://waterpump.local/api/pump/on -d '{"seconds":10}'
curl -X POST http://waterpump.local/api/pump/off
curl -X POST http://waterpump.local/api/config -d '{"interval_seconds":7200}'
```

## Configuration

Live settings are stored in **NVS** and changed over the API. Reflashing does not
reset them; `pio run -t erase` does.

[include/config.h](include/config.h) holds the wiring constants and the factory
defaults used the first time the board boots with an empty NVS partition:

**Wiring**

| Constant | Default | Meaning |
|---|---|---|
| `RELAY_GPIO` | `27` | GPIO driving the relay's IN pin |
| `RELAY_ACTIVE_LOW` | `1` | A LOW level energises the relay |
| `PRESENCE_GPIO` | `33` | GPIO reading the presence sensor's `OUT` pin |
| `PRESENCE_ACTIVE_HIGH` | `1` | HIGH on `OUT` means someone is present |
| `LD2410_UART_TXD_GPIO` / `_RXD_GPIO` | `17` / `16` | Radar only: ESP32 pins wired to the sensor's `RX`/`TX` |

**Watering**

| Constant | Default | Meaning |
|---|---|---|
| `PUMP_ON_SECONDS` | `1200` | Run time of one scheduled cycle (20 min) |
| `PUMP_INTERVAL_SECONDS` | `3600` | Gap between cycles (1 h) |
| `PUMP_MANUAL_ON_SECONDS` | `30` | Default duration of a "pour now" request |
| `RUN_ON_FIRST_BOOT` | `1` | Water on cold boot when the schedule is active, or wait one interval |
| `PUMP_MAX_ON_SECONDS` | `1800` | Hard cap, enforced on every request (30 min) |

**Presence mode** — defaults for the API fields of the same name, see
[Presence mode](#presence-mode)

| Constant | Default | Meaning |
|---|---|---|
| `PRESENCE_SENSOR_DEFAULT` | `PRESENCE_SENSOR_PIR` | Sensor assumed on first boot |
| `PRESENCE_CONFIRM_PULSES` / `_WINDOW_SECONDS` | `1` / `10` | Start on the first pulse, or on a second one within the window |
| `PRESENCE_HOLD_SECONDS` | `15` | Decay time constant of the score |
| `PRESENCE_MAX_SESSION_SECONDS` | `300` | Longest single session |
| `PRESENCE_REST_SECONDS` | `10` | Minimum gap between two sessions |
| `PRESENCE_HOURLY_BUDGET_SECONDS` | `1200` | Presence pouring per rolling hour |
| `LD2410_MAX_GATE` | `8` | Radar only: distance gate pushed to the sensor |
| `LD2410_SENSITIVITY` | `50` | Radar only: sensitivity (0 most sensitive – 100 least) |
| `LD2410_NO_ONE_SECONDS` | `5` | Radar only: its own "gone" timeout |

**Presence mode, fixed** — not exposed over the API

| Constant | Default | Meaning |
|---|---|---|
| `PRESENCE_POLL_MS` | `200` | How often the presence task samples `OUT` |
| `PIR_WARMUP_SECONDS` | `60` | PIR only: `OUT` not acted on after boot |
| `PRESENCE_TH_OFF` / `PRESENCE_X_MAX` | `0.3` / `4.0` | Score off-threshold and cap |
| `PRESENCE_MIN_PULSE_MS` | `400` | `OUT` must be HIGH this long to count |
| `PRESENCE_REEMIT_MS` | `2000` | A pulse still HIGH counts again this often |
| `PRESENCE_BLANK_MS` | `1000` | `OUT` ignored after the pump switches |
| `PRESENCE_STUCK_SECONDS` | `600` | `OUT` HIGH this long is a stuck sensor |

WiFi credentials go in `include/wifi_secrets.h`, which is gitignored. Copy the
template and fill it in:

```sh
cp include/wifi_secrets.h.example include/wifi_secrets.h
```

Board-level settings (flash size, UART console, CPU frequency) are in
[sdkconfig.defaults](sdkconfig.defaults), and the flash layout in
[partitions.csv](partitions.csv) — the default single-app table caps the application
at 1 MB, which this build nearly fills once WiFi, HTTP and mDNS are linked in.

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

## Tests

The presence-mode decision ([src/presence_logic.c](src/presence_logic.c)) has
host-side tests covering a cat walking past, drinking, calm and repeated visits,
the two-pulse confirmation, a stuck sensor, the app's stop button, the hourly
budget, and mode changes. No board needed:

```sh
gcc -std=c11 -Wall -Wextra -Iinclude test/test_presence_logic.c src/presence_logic.c -lm \
    -o /tmp/test_presence_logic && /tmp/test_presence_logic
```

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

### Presence sensor checkout (`presence` mode only)

1. Wire the sensor as in
   [Adding the presence sensor](#adding-the-presence-sensor-presence-mode-only) and
   make sure `presence_sensor` names it (restart after changing it). The boot log
   names the active sensor: `presence: pir on GPIO 33 ...`.
2. Radar only: in the monitor, expect a `ld2410: UART up on port 2 ...` line, and,
   a moment later, `pushed gate=... sensitivity=... no_one=...` confirming the sensor
   acked the boot-time config push. A timeout there means check the `TX`/`RX` wiring
   (they cross: ESP32 TX → sensor RX) before anything else.
3. Wave a hand in front of the sensor and poll status:
   `curl http://waterpump.local/api/status` — `presence_detected` should flip to
   `true` while your hand is there, `false` shortly after, regardless of `mode`.
4. Switch into presence mode:
   `curl -X POST http://waterpump.local/api/config -d '{"mode":"presence"}'`
   (after a reboot, wait out the PIR's warm-up first — `presence_warming_up: false`).
5. Wave once. Expect a relay click and a `session start` log line within about half a
   second, `presence_score` around 1–2 in the status, and `session end` some 20–30 s
   after the pulse ends.
6. Keep waving every few seconds: the pump must keep running and `presence_score`
   stay up; stop, and it must end about half a minute later.
7. Radar only: try tightening sensitivity to see the sensor-side push take effect:
   `curl -X POST http://waterpump.local/api/config -d '{"presence_sensitivity":10}'`
   should make a distant wave less likely to register.
8. Set `mode` back to whatever you actually want.

## Note on timing

The interval is a plain periodic timer measured from boot, not a time of day, and it
restarts from zero after a power cut. Drift is a few seconds per hour — irrelevant
for watering. If you ever need "every day at 08:00", that means SNTP (the board is on
WiFi now, so this is no longer far off) or an external RTC.
