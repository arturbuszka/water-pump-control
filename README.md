# waterpumpmod

Interval-based controller for a 5V water pump, built on a **Seeed Studio XIAO ESP32-C3**
and a single-channel opto-isolated relay module (10A/250VAC contacts, 5V coil).

No WiFi, no UI — the pump switches on every N minutes and runs for N seconds.
Simple enough to leave running unattended.

## Wiring

```
USB 5V ──┬──────────────────────── XIAO  5V pad
         │
         ├──────────────────────── RELAY  VCC
         │
         └── CUT the pump's + wire ──► RELAY  COM
                                       RELAY  NO ──► pump  +

GND ─────┬──────────────────────── XIAO  GND
         ├──────────────────────── RELAY  GND
         └──────────────────────── pump  −      (leave intact)

XIAO GPIO4 ─────────────────────── RELAY  IN
```

### Rules worth following

- **Cut only the pump's positive (+) wire.** Ground must stay continuous and shared
  between the XIAO, the relay and the pump. Without a common ground the IN signal
  has no reference and the relay behaves erratically.
- **Use NO, not NC.** With the contacts normally open, a power loss or MCU reset
  leaves the pump off. On NC it would be the reverse — a reset would mean flooding.
- The module is **active-LOW**: pulling IN low closes COM↔NO.

### Two things that may not work on the first try

**GPIO4 outputs 3.3V, the module runs on 5V.** Most opto-isolated modules trigger
fine at 3.3V, but not all of them. If the dry run produces no click, add a driver:

```
GPIO4 ──[ 1 kΩ ]── base   2N2222
                   emitter ── GND
                   collector ── relay IN
```
Note the transistor **inverts** the signal, so set `RELAY_ACTIVE_LOW = false` if you
add one.

**Brownout.** The pump and the XIAO draw from the same USB supply. If starting the
pump resets the board, add a 470–1000 µF capacitor across the 5V rail, or power the
pump separately while keeping the ground shared.

## Configuration

Everything lives in [include/config.h](include/config.h):

| Constant | Default | Meaning |
|---|---|---|
| `RELAY_PIN` | `4` | GPIO driving the relay's IN pin |
| `RELAY_ACTIVE_LOW` | `true` | A LOW level energises the relay |
| `PUMP_ON_SECONDS` | `10` | Pump run time per cycle |
| `PUMP_INTERVAL_MINUTES` | `360` | Gap between cycles (6 h) |
| `RUN_ON_BOOT` | `false` | Water immediately on boot |
| `PUMP_MAX_ON_SECONDS` | `60` | Hard cap — clamps `PUMP_ON_SECONDS` |

## Building

```sh
pio run                  # compile
pio run -t upload        # flash
pio device monitor       # logs, 115200
```

If the board won't enter bootloader mode: hold **BOOT**, plug in USB-C, release BOOT.

## Dry run — do this before connecting the pump

1. Connect **only** the XIAO and the relay (VCC / GND / IN). Leave COM and NO empty.
2. Temporarily set in `config.h`:
   `PUMP_ON_SECONDS = 3`, `PUMP_INTERVAL_MINUTES = 0`, `RUN_ON_BOOT = true`.
3. `pio run -t upload && pio device monitor`
4. Expected: an audible **click**, the module's LED on for 3 s, then a click back —
   with the serial log matching the clicks.
   - No clicks → 3.3V isn't enough, add the transistor driver above.
   - Clicks inverted relative to the log → flip `RELAY_ACTIVE_LOW`.
5. Put a multimeter in continuity mode across **COM–NO**: open when off, closed when
   on. This confirms you're using the right contact pair.
6. Only now wire in the pump and watch one full cycle.
7. Restore your real `PUMP_ON_SECONDS` / `PUMP_INTERVAL_MINUTES`, set
   `RUN_ON_BOOT = false`, and flash again.

## Note on timing

Timing is based on `millis()`, so it drifts and resets on power loss. For watering
plants that's irrelevant. If you ever need an actual time of day, you'd have to add
WiFi + NTP or an RTC module.
