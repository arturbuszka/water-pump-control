#pragma once

// ---- Hardware ----

// GPIO driving the relay module's IN pin.
#define RELAY_GPIO 27

// Opto-isolated module: a LOW on IN closes COM-NO (pump runs).
// If the relay clicks opposite to what the logs say, flip this to 0.
#define RELAY_ACTIVE_LOW 1

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
