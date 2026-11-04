#include "presence_logic.h"

#include <math.h>
#include <string.h>

// x is compared against thresholds that sums of exactly 1.0 are meant to hit;
// keep float rounding from missing them by a hair.
#define TH_EPSILON 0.001f

void presence_logic_init(presence_logic_t *st, int64_t now_ms) {
	memset(st, 0, sizeof(*st));
	st->rearmed = true;
	st->last_ms = now_ms;
	// No rest owed before the first session.
	st->session_end_ms = INT64_MIN / 2;
	for (int i = 0; i < PRESENCE_BUDGET_SLOTS; i++) {
		st->budget_slot_minute[i] = -1;
	}
}

static void budget_add(presence_logic_t *st, int64_t now_ms, uint32_t ms) {
	int64_t minute = now_ms / 60000;
	int slot = (int)(minute % PRESENCE_BUDGET_SLOTS);
	if (st->budget_slot_minute[slot] != minute) {
		st->budget_slot_minute[slot] = minute;
		st->budget_slot_ms[slot] = 0;
	}
	st->budget_slot_ms[slot] += ms;
}

uint32_t presence_logic_budget_used_ms(const presence_logic_t *st, int64_t now_ms) {
	int64_t minute = now_ms / 60000;
	uint32_t used = 0;
	for (int i = 0; i < PRESENCE_BUDGET_SLOTS; i++) {
		if (st->budget_slot_minute[i] >= 0 && minute - st->budget_slot_minute[i] < PRESENCE_BUDGET_SLOTS) {
			used += st->budget_slot_ms[i];
		}
	}
	return used;
}

float presence_logic_th_on(uint32_t pulses, uint32_t window_ms, uint32_t hold_ms) {
	if (pulses <= 1) {
		return 1.0f - TH_EPSILON;
	}
	return 1.0f + expf(-(float)window_ms / (float)hold_ms) - TH_EPSILON;
}

static void end_session(presence_logic_t *st, const presence_logic_params_t *p, int64_t now_ms) {
	st->session = false;
	st->session_end_ms = now_ms;
	st->rearmed = st->x < p->th_off;
}

presence_action_t presence_logic_step(presence_logic_t *st, const presence_logic_params_t *p,
                                      int64_t now_ms, const presence_logic_input_t *in) {
	int64_t dt = now_ms - st->last_ms;
	if (dt < 0) {
		dt = 0;
	}
	st->last_ms = now_ms;

	st->x *= expf(-(float)dt / (float)p->hold_ms);
	if (st->session && in->pump_ours) {
		budget_add(st, now_ms, (uint32_t)dt);
	}

	// The relay and pump can couple a spike into the sensor lead as they switch.
	if (in->pump_any != st->prev_pump_any) {
		st->prev_pump_any = in->pump_any;
		st->blank_until_ms = now_ms + p->blank_ms;
	}

	bool evidence = false;
	if (in->out) {
		if (!st->was_high) {
			st->high_since_ms = now_ms;
			st->next_emit_ms = now_ms + p->min_pulse_ms;
		}
		if (now_ms >= st->next_emit_ms) {
			evidence = true;
			st->next_emit_ms += p->reemit_ms;
		}
		if (now_ms - st->high_since_ms >= p->stuck_ms) {
			st->fault = true;
		}
	} else {
		st->fault = false;
	}
	st->was_high = in->out;

	if (!in->enabled) {
		st->x = 0.0f;
		if (st->session) {
			end_session(st, p, now_ms);
			return PRESENCE_ACTION_STOP;
		}
		st->rearmed = true;
		return PRESENCE_ACTION_NONE;
	}

	if (evidence && !st->fault && now_ms >= st->blank_until_ms) {
		st->x += 1.0f;
		if (st->x > p->x_max) {
			st->x = p->x_max;
		}
	}
	if (st->x < p->th_off) {
		st->rearmed = true;
	}

	bool budget_left = presence_logic_budget_used_ms(st, now_ms) < p->budget_ms;

	if (st->session) {
		// Stopped from outside (the app's stop button, the pump's own cap), or
		// the start request never turned into a run. Give the pump task a
		// moment to pick the request up first.
		if (!in->pump_ours && now_ms - st->session_start_ms > 1500) {
			end_session(st, p, now_ms);
			return PRESENCE_ACTION_NONE;
		}
		if (st->x < p->th_off || st->fault || !budget_left ||
		    now_ms - st->session_start_ms >= p->max_session_ms) {
			end_session(st, p, now_ms);
			return PRESENCE_ACTION_STOP;
		}
		return PRESENCE_ACTION_NONE;
	}

	if (st->x >= p->th_on && st->rearmed && !st->fault && budget_left && !in->pump_any &&
	    now_ms - st->session_end_ms >= p->rest_ms) {
		st->session = true;
		st->rearmed = false;
		st->session_start_ms = now_ms;
		return PRESENCE_ACTION_START;
	}
	return PRESENCE_ACTION_NONE;
}
