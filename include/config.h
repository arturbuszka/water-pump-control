#pragma once

// ---- Hardware ----

// GPIO driving the relay module's IN pin.
#define RELAY_GPIO 27

// Opto-isolated module: a LOW on IN closes COM-NO (pump runs).
// If the relay clicks opposite to what the logs say, flip this to 0.
#define RELAY_ACTIVE_LOW 1

// GPIO reading the presence sensor's OUT pin: a simple HIGH/LOW "someone is
// there" signal, used by the presence trigger task. Both supported sensors
// (see presence_sensor_type_t) put it on this same pin.
#define PRESENCE_GPIO 33

// OUT is active-HIGH (HIGH = target present) on both the PIR and the stock
// LD2410C firmware. Flip to 0 if the dry run shows the opposite.
#define PRESENCE_ACTIVE_HIGH 1

// Sensor assumed on first boot with an empty NVS; changed later over the API
// (`presence_sensor`), which takes effect after a restart.
#define PRESENCE_SENSOR_DEFAULT PRESENCE_SENSOR_PIR

// PIR only: how long after boot OUT is ignored for triggering. PIR modules
// settle for 30-60 s after power-up and can pulse HIGH meanwhile, which would
// otherwise pour on every reboot in presence mode.
#define PIR_WARMUP_SECONDS 60

// LD2410C only: the sensor's TX/RX pins, wired to a second UART (distinct from the UART0
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

// ---- Presence mode: factory defaults ----
//
// How presence mode turns sensor pulses into pouring - the model is described
// in include/presence_logic.h. These are changed over the API like the cycle
// settings above.

// 1 = pour on the first pulse; 2 = wait for a second one within
// PRESENCE_CONFIRM_WINDOW_SECONDS (ignores a cat just walking past, at the
// cost of a few seconds' delay).
#define PRESENCE_CONFIRM_PULSES 1
#define PRESENCE_CONFIRM_WINDOW_SECONDS 10

// Decay time constant of the presence score: how long a pulse is "remembered".
// After the last pulse the pump keeps going for hold * ln(score / 0.3) -
// roughly 18 s after a single pulse, up to ~39 s after a drinking session.
#define PRESENCE_HOLD_SECONDS 15

// One presence session never pours longer than this.
#define PRESENCE_MAX_SESSION_SECONDS (60 * 5)

// Minimum gap between the end of one session and the start of the next.
#define PRESENCE_REST_SECONDS 10

// Presence pouring allowed per rolling hour, whatever the sensor says.
#define PRESENCE_HOURLY_BUDGET_SECONDS (60 * 20)

// ---- Presence mode: fixed tuning ----
//
// Settled in simulation (score thresholds) or by the hardware (timings); not
// exposed over the API.

// How often the presence task samples PRESENCE_GPIO.
#define PRESENCE_POLL_MS 200

// Score hysteresis: a session ends below PRESENCE_TH_OFF; the score is capped
// at PRESENCE_X_MAX so a long visit can't keep the pump going minutes after.
#define PRESENCE_TH_OFF 0.3f
#define PRESENCE_X_MAX 4.0f

// OUT must stay HIGH this long to count at all - filters spikes, while every
// real PIR or radar pulse lasts far longer.
#define PRESENCE_MIN_PULSE_MS 400

// While OUT stays HIGH it counts as a fresh pulse this often, so a sensor that
// holds OUT HIGH through continuous motion keeps the score up.
#define PRESENCE_REEMIT_MS 2000

// OUT is ignored this long after the pump switches on or off - relay and pump
// can couple a spike into the sensor lead.
#define PRESENCE_BLANK_MS 1000

// OUT HIGH without a break this long is treated as a stuck sensor: the session
// stops and the sensor is ignored until OUT goes LOW again.
#define PRESENCE_STUCK_SECONDS (60 * 10)

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
