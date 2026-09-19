#pragma once

// ---- Hardware ----

// GPIO driving the relay module's IN pin.
#define RELAY_GPIO 27

// Opto-isolated module: a LOW on IN closes COM-NO (pump runs).
// If the relay clicks opposite to what the logs say, flip this to 0.
#define RELAY_ACTIVE_LOW 1

// ---- Watering cycle ----

// How long the pump runs in one cycle.
#define PUMP_ON_SECONDS (60 * 20)

// Gap between cycles, in seconds. The board deep-sleeps through this.
// 60 * 60 * 2 = 2 h. Small values are for bench testing only - see README's dry run.
#define PUMP_INTERVAL_SECONDS (60 * 60 * 1)

// Water immediately on the very first boot (power-on / reset)? 0 means the first
// cycle happens one full interval later, so a reset loop can't flood the plants.
#define RUN_ON_FIRST_BOOT 1

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
