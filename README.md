# waterpumpmod

Interval-based controller for a 5V water pump, built on an **ESP32 DevKit board with
an ESP-WROOM-32 module** and a single-channel opto-isolated relay module
(10A/250VAC contacts, 5V coil).

Written against **ESP-IDF** with deep sleep between cycles, so it can run from a
battery: roughly 10 µA at the module versus ~50 mA for an always-awake loop.

Caveat for this board: a DevKit's USB-UART bridge and LDO keep drawing current even
while the chip sleeps, so measured at the 5V pin you'll see a few mA, not µA. For a
genuinely long battery run you'd power the WROOM module directly rather than through
the DevKit's 5V pin.

No WiFi, no UI — the board wakes on a timer, runs the pump for N seconds, and goes
back to sleep.

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

## Configuration

Everything lives in [include/config.h](include/config.h):

| Constant | Default | Meaning |
|---|---|---|
| `RELAY_GPIO` | `27` | GPIO driving the relay's IN pin (must be RTC-capable) |
| `RELAY_ACTIVE_LOW` | `1` | A LOW level energises the relay |
| `PUMP_ON_SECONDS` | `10` | Pump run time per cycle |
| `PUMP_INTERVAL_SECONDS` | `21600` | Sleep between cycles (21600 s = 6 h) |
| `RUN_ON_FIRST_BOOT` | `0` | Water on cold boot, or wait one interval |
| `PUMP_MAX_ON_SECONDS` | `60` | Hard cap — clamps `PUMP_ON_SECONDS` |

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

1. Connect **only** the ESP32 and the relay (VCC / GND / IN). Leave COM and NO empty.
2. Temporarily set in `config.h`:
   `PUMP_ON_SECONDS 3`, `PUMP_INTERVAL_SECONDS 10`, `RUN_ON_FIRST_BOOT 1`.
3. `pio run -t upload && pio device monitor`
4. Expected: an audible **click**, the module's LED on for 3 s, a click back, then a
   sleep message — and a fresh boot banner ten seconds later.
   - No clicks → 3.3V isn't enough, add the transistor driver above.
   - Clicks inverted relative to the log → flip `RELAY_ACTIVE_LOW`.
5. Put a multimeter in continuity mode across **COM–NO**: open when off, closed when
   on. This confirms you're using the right contact pair.
6. **Leave it sleeping and watch the relay for a minute.** It must stay silent. A
   click during sleep means the pin isn't being held — check `rtc_gpio_hold_en()`.
7. Only now wire in the pump and watch one full cycle.
8. Restore your real `PUMP_ON_SECONDS` / `PUMP_INTERVAL_SECONDS`, set
   `RUN_ON_FIRST_BOOT 0`, and flash again.

## Note on timing

The wake timer runs off the RTC oscillator, which drifts by a few percent with
temperature — over a 6-hour sleep that's minutes, which does not matter for watering
plants. Sleep time also does not accumulate across power loss: unplug the board and
the schedule restarts from the cold boot. If you ever need a real time of day, that
means WiFi + NTP or an external RTC.
