# waterpumpmod

Controller for a 5V water pump powering a **cat drinking fountain**, built on an
**ESP32 DevKit board with an ESP-WROOM-32 module** and a single-channel
opto-isolated relay module (10A/250VAC contacts, 5V coil).

Two trigger modes, picked with `PUMP_TRIGGER_MODE` in
[include/config.h](include/config.h):

- **`PUMP_TRIGGER_INTERVAL`** — the pump runs on a fixed schedule, regardless of
  whether a cat is around.
- **`PUMP_TRIGGER_PRESENCE`** — an **HLK-LD2410C 24GHz radar presence sensor**
  triggers the pump when a cat approaches, with a cooldown so one long visit
  doesn't retrigger it repeatedly. See [Presence mode](#presence-mode) below.

Written against **ESP-IDF** with deep sleep between cycles, so it can run from a
battery: roughly 10 µA at the module versus ~50 mA for an always-awake loop.

Caveat for this board: a DevKit's USB-UART bridge and LDO keep drawing current even
while the chip sleeps, so measured at the 5V pin you'll see a few mA, not µA. For a
genuinely long battery run you'd power the WROOM module directly rather than through
the DevKit's 5V pin.

No WiFi, no UI — the board wakes on a timer or on the presence sensor, runs the pump
for N seconds, and goes back to sleep.

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

### Adding the presence sensor (`PUMP_TRIGGER_PRESENCE` only)

```
5V  ──────────────────────────── LD2410C  VCC
GND ──────────────────────────── LD2410C  GND (shared with ESP32 and relay)
ESP32 GPIO33 ───────────────────LD2410C  OUT
```

- **OUT only** — the sensor's `TX`/`RX` pins are a separate UART used for richer
  data (distance, micro-motion) and configuring sensitivity/range/hold time. Not
  needed here: `OUT` alone gives a simple HIGH/LOW "cat present" signal, which is
  all `PUMP_TRIGGER_PRESENCE` reads.
- `OUT` must go to an **RTC-capable GPIO**, same constraint as the relay pin — it's
  used as an `ext0` deep-sleep wakeup source. GPIO33 is RTC-capable and free (the
  relay uses GPIO27).
- Stock LD2410C firmware drives `OUT` active-HIGH (HIGH = target present). Confirm
  this in the dry run below and flip `PRESENCE_ACTIVE_HIGH` in `config.h` if not.

## How it works

A deep-sleep wake is a **fresh boot**, not a resumed program — execution starts from
`app_main()` every time, and ordinary globals are re-initialised. So there is no main
loop here; one pass through `app_main()` is one watering cycle:

```
wake (timer) → release pin hold → pump ON → wait → pump OFF → sleep again
```

Two details this forces, both in [src/main.c](src/main.c):

- **`cycle_count` lives in RTC memory** (`RTC_DATA_ATTR`). That region keeps its
  contents while the CPU is off; a normal global would reset to zero on every wake.
- **The relay pin is latched with `rtc_gpio_hold_en()` before sleeping.** Without it
  the pin floats once the digital core powers down, and a floating IN can energise the
  relay mid-sleep — the pump would run unattended until the tank or battery ran out.
  The hold survives the wake, so `app_main()` releases it with `rtc_gpio_hold_dis()`
  before driving the pin again.

  On the classic ESP32 this latch lives in the RTC domain. That has two consequences:
  **the relay pin must be an RTC-capable GPIO** (GPIO27 is; GPIO16–19, 21–23 and 25
  are not), and the RTC peripheral domain has to be kept powered with
  `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)` — otherwise the
  hold is dropped the instant the chip powers down.

### Presence mode

In `PUMP_TRIGGER_PRESENCE`, `app_main_presence()` in [src/main.c](src/main.c) is a
small state machine, with `presence_state` also living in RTC memory so it survives
the deep-sleep reboot cycle:

```
ARMED (ext0 wake on LD2410C OUT)
  └─ wake → confirmed presence? ──yes──► pump ON → wait → pump OFF → COOLDOWN
                    │no
                    └─► stay ARMED (was noise, ignore)

COOLDOWN (timer wake only, ext0 disabled)
  └─ cooldown elapsed → back to ARMED
```

- **ARMED**: deep-sleeps with `esp_sleep_enable_ext0_wakeup()` on the sensor's `OUT`
  pin — any level change wakes the board immediately, so a visit is never missed
  even though the CPU is asleep almost all the time.
- On wake, a few debounced reads of `OUT` confirm it's a real detection and not
  noise at the edge of the sensor's range, before running the pump.
- **COOLDOWN**: right after watering, `ext0` is disabled and the board sleeps on a
  plain timer for `PUMP_COOLDOWN_SECONDS`. A cat that lingers at the fountain (or
  keeps wandering in and out of range) can't retrigger the pump during this window.
  Once it elapses, the board re-arms and goes back to listening for presence.

## Configuration

Everything lives in [include/config.h](include/config.h):

| Constant | Default | Meaning |
|---|---|---|
| `RELAY_GPIO` | `27` | GPIO driving the relay's IN pin (must be RTC-capable) |
| `RELAY_ACTIVE_LOW` | `1` | A LOW level energises the relay |
| `PRESENCE_GPIO` | `33` | GPIO reading the LD2410C's `OUT` pin (must be RTC-capable, `PUMP_TRIGGER_PRESENCE` only) |
| `PRESENCE_ACTIVE_HIGH` | `1` | HIGH on `OUT` means a cat is present |
| `PUMP_TRIGGER_MODE` | `PUMP_TRIGGER_PRESENCE` | `PUMP_TRIGGER_INTERVAL` or `PUMP_TRIGGER_PRESENCE` |
| `PUMP_ON_SECONDS` | `1200` | Pump run time per cycle |
| `PUMP_INTERVAL_SECONDS` | `3600` | `PUMP_TRIGGER_INTERVAL` only: sleep between cycles |
| `RUN_ON_FIRST_BOOT` | `1` | `PUMP_TRIGGER_INTERVAL` only: water on cold boot, or wait one interval |
| `PUMP_COOLDOWN_SECONDS` | `5`\* | `PUMP_TRIGGER_PRESENCE` only: minimum gap between presence-triggered waterings |
| `PRESENCE_DEBOUNCE_SAMPLES` / `PRESENCE_DEBOUNCE_MS` | `3` / `75` | Noise filtering for a presence wake |
| `PUMP_MAX_ON_SECONDS` | `1800` | Hard cap — clamps `PUMP_ON_SECONDS` |

\* `PUMP_COOLDOWN_SECONDS` is currently set to a short bench-testing value (`5`); the
intended default (`60 * 30` = 30 min) is left commented out above it in `config.h` —
restore that before running unattended.

Board-level settings (4MB flash size, UART console) are in
[sdkconfig.defaults](sdkconfig.defaults). ESP-IDF is configured through Kconfig, so
those belong there rather than in `build_flags`.

## Building

```sh
pio run                  # compile
pio run -t upload        # flash
pio device monitor       # logs, 115200
pio run -t menuconfig    # ESP-IDF configuration UI
```

Most DevKit boards auto-reset into the bootloader via DTR/RTS. If yours doesn't: hold
**BOOT (IO0)**, tap **EN/RST**, release BOOT.

> The console runs over UART0, through the board's USB-UART bridge (CP2102 or CH340).
> The bridge stays powered from USB, so unlike a native-USB board the monitor keeps
> its connection during deep sleep — the port just goes quiet until the next wake.
> You may need the driver for your bridge chip if the port doesn't appear.

## Dry run — do this before connecting the pump

Relay/pump checkout (both trigger modes):

1. Connect **only** the ESP32 and the relay (VCC / GND / IN). Leave COM and NO empty.
2. If testing `PUMP_TRIGGER_INTERVAL`, temporarily set in `config.h`:
   `PUMP_ON_SECONDS 3`, `PUMP_INTERVAL_SECONDS 10`, `RUN_ON_FIRST_BOOT 1`. If testing
   `PUMP_TRIGGER_PRESENCE`, see the sensor checkout below instead — presence mode
   doesn't water on cold boot by design.
3. `pio run -t upload && pio device monitor`
4. Expected: an audible **click**, the module's LED on for 3 s, a click back, then a
   sleep message — and (interval mode) a fresh boot banner ten seconds later.
   - No clicks → 3.3V isn't enough, add the transistor driver above.
   - Clicks inverted relative to the log → flip `RELAY_ACTIVE_LOW`.
5. Put a multimeter in continuity mode across **COM–NO**: open when off, closed when
   on. This confirms you're using the right contact pair.
6. **Leave it sleeping and watch the relay for a minute.** It must stay silent. A
   click during sleep means the pin isn't being held — check `rtc_gpio_hold_en()`.
7. Only now wire in the pump and watch one full cycle.
8. Restore your real `PUMP_ON_SECONDS` / `PUMP_INTERVAL_SECONDS`, set
   `RUN_ON_FIRST_BOOT 0` (interval mode), and flash again.

Presence sensor checkout (`PUMP_TRIGGER_PRESENCE` only), once the relay/pump side is
confirmed working:

1. Wire the LD2410C as in [Adding the presence sensor](#adding-the-presence-sensor-pump_trigger_presence-only).
   Leave the pump disconnected from the relay for this step.
2. `pio run -t upload && pio device monitor`.
3. Wave a hand in front of the sensor before it goes to sleep and watch the log —
   confirm `OUT` reads as expected. If detection looks inverted, flip
   `PRESENCE_ACTIVE_HIGH`.
4. Let it go to sleep (ARMED), then trigger presence again. Expected: an immediate
   wake (no waiting for a timer), a "presence confirmed" log line, a relay click,
   then a "cooldown" log line.
5. While in cooldown, trigger presence again (e.g. keep your hand there) and confirm
   the board **stays asleep** — no click, no log line — until
   `PUMP_COOLDOWN_SECONDS` elapses and it re-arms.
6. Wire in the pump and watch one full detect → water → cooldown → re-arm cycle.
7. If you used a short `PUMP_COOLDOWN_SECONDS` for this test, restore the intended
   value before running unattended.

## Note on timing

The wake timer runs off the RTC oscillator, which drifts by a few percent with
temperature — over a multi-hour sleep that's minutes, which doesn't matter for
`PUMP_INTERVAL_SECONDS` or `PUMP_COOLDOWN_SECONDS`. Sleep time also does not
accumulate across power loss: unplug the board and the schedule/cooldown restarts
from the cold boot. If you ever need a real time of day, that means WiFi + NTP or an
external RTC.
