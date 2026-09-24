#pragma once

// ---- Hardware ----

// GPIO driving the relay module's IN pin.
#define RELAY_GPIO 27

// Opto-isolated module: a LOW on IN closes COM-NO (pump runs).
// If the relay clicks opposite to what the logs say, flip this to 0.
#define RELAY_ACTIVE_LOW 1

// GPIO reading the HLK-LD2410C's OUT pin (RTC-capable, distinct from RELAY_GPIO -
// used as an ext0 deep-sleep wakeup source in PUMP_TRIGGER_PRESENCE mode).
#define PRESENCE_GPIO 33

// The LD2410C's OUT pin is active-HIGH (HIGH = target present) on stock firmware.
// Flip to 0 if the dry run shows the opposite.
#define PRESENCE_ACTIVE_HIGH 1

// ---- Trigger mode ----

// What starts a watering cycle.
//   PUMP_TRIGGER_INTERVAL - the original behaviour: wake on a fixed timer.
//   PUMP_TRIGGER_PRESENCE - wake when the LD2410C reports the cat is present.
#define PUMP_TRIGGER_INTERVAL 0
#define PUMP_TRIGGER_PRESENCE 1
#define PUMP_TRIGGER_MODE PUMP_TRIGGER_PRESENCE

// ---- Watering cycle ----

// How long the pump runs in one cycle.
#define PUMP_ON_SECONDS (60 * 20)

// PUMP_TRIGGER_INTERVAL only: gap between cycles, in seconds. The board
// deep-sleeps through this. 60 * 60 * 2 = 2 h. Small values are for bench
// testing only - see README's dry run.
#define PUMP_INTERVAL_SECONDS (60 * 60 * 1)

// PUMP_TRIGGER_INTERVAL only: water immediately on the very first boot
// (power-on / reset)? 0 means the first cycle happens one full interval later,
// so a reset loop can't flood the plants.
#define RUN_ON_FIRST_BOOT 1

// PUMP_TRIGGER_PRESENCE only: minimum gap between two presence-triggered
// waterings, in seconds. After a cycle the board ignores the sensor and
// deep-sleeps on a timer for this long before re-arming ext0 wakeup - this is
// what stops one long visit (or a cat that won't leave) from re-triggering the
// pump over and over. 60 * 30 = 30 min.
// #define PUMP_COOLDOWN_SECONDS (60 * 30)
#define PUMP_COOLDOWN_SECONDS 5

// PUMP_TRIGGER_PRESENCE only: consecutive PRESENCE_GPIO reads, PRESENCE_DEBOUNCE_MS
// apart, that must all agree before a wakeup is treated as a real detection
// rather than noise at the edge of the sensor's range.
#define PRESENCE_DEBOUNCE_SAMPLES 3
#define PRESENCE_DEBOUNCE_MS 75

// ---- Safety ----

// Hard cap on run time. If PUMP_ON_SECONDS is ever set to something absurd,
// the pump still shuts off here. 60 * 30 = 30 min - twice the normal run, so a
// typo costs a longer cycle rather than a pump left running for hours.
#define PUMP_MAX_ON_SECONDS (60 * 30)

// ---- Derived ----

// Level that keeps the relay de-energised. Held on the pin through deep sleep.
#define RELAY_LEVEL_OFF (RELAY_ACTIVE_LOW ? 1 : 0)
#define RELAY_LEVEL_ON  (RELAY_ACTIVE_LOW ? 0 : 1)

#define PUMP_ON_SECONDS_EFFECTIVE \
	((PUMP_ON_SECONDS) > (PUMP_MAX_ON_SECONDS) ? (PUMP_MAX_ON_SECONDS) : (PUMP_ON_SECONDS))
