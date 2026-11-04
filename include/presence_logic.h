#pragma once

#include <stdbool.h>
#include <stdint.h>

// The presence-mode decision, kept free of ESP-IDF so it can be unit-tested on
// a PC (see test/test_presence_logic.c). presence.c feeds it one sample per
// poll and carries out what it returns.
//
// The model: `x` is a leaky integrator of motion evidence. Every pulse on OUT
// adds 1 (capped at x_max), and between pulses x decays with time constant
// hold_ms:
//
//     x[k] = min(x[k-1] * exp(-dt / hold) + e[k], x_max)
//
// e[k] is 1 once OUT has been HIGH for min_pulse_ms (a glitch filter), and
// again every reemit_ms it stays HIGH - so a sensor that holds OUT HIGH while
// something keeps moving (a retriggering PIR, the radar) keeps counting as
// fresh evidence instead of a single pulse.
//
// The pump follows x with hysteresis: a session starts at x >= th_on and ends
// at x < th_off. After the last pulse it therefore keeps pouring for
// hold * ln(x_peak / th_off): briefly for a single pulse (a cat walking past),
// longer after a run of pulses (a cat drinking). Around that sit hard limits -
// a maximum session, a rest after each one, an hourly budget, and a fault when
// OUT is stuck HIGH.

typedef struct {
	uint32_t hold_ms;         // decay time constant of x
	float th_on;              // start a session at x >= th_on
	float th_off;             // end it at x < th_off
	float x_max;              // cap on x; bounds the pour after the last pulse
	uint32_t min_pulse_ms;    // OUT must be HIGH this long to count at all
	uint32_t reemit_ms;       // a pulse still HIGH counts again this often
	uint32_t blank_ms;        // ignore OUT this long after the pump switches
	uint32_t rest_ms;         // minimum gap between two sessions
	uint32_t max_session_ms;  // a session never runs longer
	uint32_t budget_ms;       // presence pouring allowed per rolling hour
	uint32_t stuck_ms;        // OUT HIGH this long without a break is a fault
} presence_logic_params_t;

// One sample, as seen by the caller.
typedef struct {
	bool out;          // sensor OUT reads "present"
	bool enabled;      // presence mode is on and the sensor is past warm-up
	bool pump_any;     // the pump is running, for whatever reason
	bool pump_ours;    // ... and it is running a presence session
} presence_logic_input_t;

typedef enum {
	PRESENCE_ACTION_NONE,
	PRESENCE_ACTION_START,  // request a presence run of max_session_ms
	PRESENCE_ACTION_STOP,   // stop the presence run in progress
} presence_action_t;

#define PRESENCE_BUDGET_SLOTS 60  // one per minute of the rolling hour

typedef struct {
	float x;
	bool session;
	// A new session may start only after x has dropped below th_off since the
	// last one ended - so a session cut short (max length, budget, the app's
	// stop button) does not restart while the same cat is still there.
	bool rearmed;
	bool fault;  // OUT stuck HIGH; cleared when it goes LOW
	bool was_high;
	int64_t last_ms;
	int64_t high_since_ms;
	int64_t next_emit_ms;
	int64_t blank_until_ms;
	bool prev_pump_any;
	int64_t session_start_ms;
	int64_t session_end_ms;
	// Presence pouring per minute, for the hourly budget.
	uint32_t budget_slot_ms[PRESENCE_BUDGET_SLOTS];
	int64_t budget_slot_minute[PRESENCE_BUDGET_SLOTS];
} presence_logic_t;

void presence_logic_init(presence_logic_t *st, int64_t now_ms);

// Advances the model to `now_ms` with sample `in` and says what to do.
presence_action_t presence_logic_step(presence_logic_t *st, const presence_logic_params_t *p,
                                      int64_t now_ms, const presence_logic_input_t *in);

// Presence pouring in the last hour, in milliseconds.
uint32_t presence_logic_budget_used_ms(const presence_logic_t *st, int64_t now_ms);

// th_on for "start after `pulses` pulses within `window_ms`" (1 or 2): two
// pulses window_ms apart leave x at 1 + exp(-window / hold).
float presence_logic_th_on(uint32_t pulses, uint32_t window_ms, uint32_t hold_ms);
