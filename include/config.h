#pragma once

// ---- Hardware ----

// GPIO driving the relay module's IN pin.
#define RELAY_GPIO 27

// Opto-isolated module: a LOW on IN closes COM-NO (pump runs).
// If the relay clicks opposite to what the logs say, flip this to 0.
#define RELAY_ACTIVE_LOW 1

// GPIO reading the HLK-LD2410C's OUT pin: a simple HIGH/LOW "someone is
// there" signal, used by the presence trigger task.
#define PRESENCE_GPIO 33

// The LD2410C's OUT pin is active-HIGH (HIGH = target present) on stock
// firmware. Flip to 0 if the dry run shows the opposite.
#define PRESENCE_ACTIVE_HIGH 1

// The sensor's TX/RX pins, wired to a second UART (distinct from the UART0
// console) so the board can push sensitivity/range configuration into it.
// Wiring: ESP32 TXD -> LD2410C RX, ESP32 RXD -> LD2410C TX (crossed, as usual).
#define LD2410_UART_PORT UART_NUM_2
#define LD2410_UART_TXD_GPIO 17
#define LD2410_UART_RXD_GPIO 16
#define LD2410_UART_BAUD_RATE 256000

// ---- Watering cycle: factory defaults ----
//
// These are only the values used the first time the board boots with an empty
// NVS partition. After that the live settings come from NVS and are changed
// over the REST API - see include/pump_config.h. Reflashing does not reset
// them; erase the flash (`pio run -t erase`) to get back to these.

// How long the pump runs in one scheduled cycle.
#define PUMP_ON_SECONDS (60 * 20)

// Gap between scheduled cycles, in seconds. 60 * 60 = 1 h.
#define PUMP_INTERVAL_SECONDS (60 * 60 * 1)

// How long a "pour now" request from the app runs when it does not name its own
// duration - the cat-at-the-bowl case, which wants a short pour, not a cycle.
#define PUMP_MANUAL_ON_SECONDS 30

// Water immediately on the very first boot? 0 means the first cycle happens one
// full interval later, so a reset loop can't flood the plants.
#define RUN_ON_FIRST_BOOT 1

// PUMP_MODE_PRESENCE only: minimum gap between two presence-triggered
// waterings, in seconds. Stops one long visit (or a cat that won't leave)
// from re-triggering the pump over and over. 60 * 30 = 30 min.
#define PUMP_PRESENCE_COOLDOWN_SECONDS (60 * 30)

// PUMP_MODE_PRESENCE only: consecutive PRESENCE_GPIO reads, PRESENCE_DEBOUNCE_MS
// apart, that must all agree before a detection is treated as real rather than
// noise at the edge of the sensor's range.
#define PRESENCE_DEBOUNCE_SAMPLES 3
#define PRESENCE_DEBOUNCE_MS 75

// How often the presence task polls PRESENCE_GPIO while armed.
#define PRESENCE_POLL_MS 200

// ---- LD2410C radar tuning: factory defaults ----
//
// Pushed into the sensor over its UART at boot and whenever changed over the
// REST API - see include/pump_config.h. The sensor itself does not persist
// across firmware reflashes in a way we rely on, so this always runs at boot.

// Distance gates 0..8 (each ~0.75 m), how far out the sensor should look.
#define LD2410_MAX_GATE 8

// Sensitivity per gate, 0 (most sensitive) - 100 (least). One value applied to
// every gate; the LD2410C protocol allows per-gate values but a single knob is
// enough for a fountain.
#define LD2410_SENSITIVITY 50

// Seconds of no detection before the sensor reports "gone". Also acts as the
// LD2410C's own hardware debounce on the losing edge.
#define LD2410_NO_ONE_SECONDS 5

// ---- Safety ----

// Hard cap on run time, enforced at runtime on every request regardless of
// where it came from. A config write asking for more than this is rejected,
// not clamped, so a typo in the app surfaces as an error instead of a surprise.
// 60 * 30 = 30 min.
#define PUMP_MAX_ON_SECONDS (60 * 30)

// ---- Derived ----

// Level that keeps the relay de-energised.
#define RELAY_LEVEL_OFF (RELAY_ACTIVE_LOW ? 1 : 0)
#define RELAY_LEVEL_ON  (RELAY_ACTIVE_LOW ? 0 : 1)

#define PUMP_ON_SECONDS_EFFECTIVE \
	((PUMP_ON_SECONDS) > (PUMP_MAX_ON_SECONDS) ? (PUMP_MAX_ON_SECONDS) : (PUMP_ON_SECONDS))
