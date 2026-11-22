// Host-side tests for presence_logic.c - no ESP32 needed:
//
//     gcc -std=c11 -Wall -Wextra -Iinclude test/test_presence_logic.c src/presence_logic.c
//         -lm -o /tmp/test_presence_logic && /tmp/test_presence_logic
//
// Each scenario drives the logic at the firmware's poll rate with a scripted
// OUT signal and a minimal pump that does what the logic asks.

#include <stdio.h>
#include <stdlib.h>

#include "presence_logic.h"

#define TICK_MS 200

static int failures;

#define CHECK(cond, ...)                                       \
	do {                                                       \
		if (!(cond)) {                                         \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);      \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
			failures++;                                        \
		}                                                      \
	} while (0)

static presence_logic_params_t defaults(void) {
	presence_logic_params_t p = {
		.hold_ms = 15000,
		.th_off = 0.3f,
		.x_max = 4.0f,
		.min_pulse_ms = 400,
		.reemit_ms = 2000,
		.blank_ms = 1000,
		.rest_ms = 10000,
		.max_session_ms = 300000,
		.budget_ms = 1200000,
		.stuck_ms = 600000,
	};
	p.th_on = presence_logic_th_on(1, 10000, p.hold_ms);
	return p;
}

// OUT as a list of [start, end) HIGH intervals in ms.
typedef struct {
	int64_t from, to;
} pulse_t;

typedef struct {
	presence_logic_t st;
	presence_logic_params_t p;
	bool pumping;          // a presence run is in progress
	bool other_pumping;    // some other source holds the pump
	int64_t run_until;     // the pump's own cap on our run
	int starts;
	int64_t on_ms;         // total presence pouring
	int64_t first_on, last_off;
	bool enabled;
} sim_t;

static void sim_init(sim_t *s, presence_logic_params_t p) {
	*s = (sim_t){.p = p, .first_on = -1, .last_off = -1, .enabled = true};
	presence_logic_init(&s->st, 0);
}

static bool out_at(const pulse_t *pulses, int n, int64_t t) {
	for (int i = 0; i < n; i++) {
		if (t >= pulses[i].from && t < pulses[i].to) return true;
	}
	return false;
}

// Runs [from, to) ms. `user_stop_at` >= 0 presses the app's stop button then.
static void sim_run(sim_t *s, const pulse_t *pulses, int n, int64_t from, int64_t to,
                    int64_t user_stop_at) {
	for (int64_t t = from; t < to; t += TICK_MS) {
		if (s->pumping && t >= s->run_until) s->pumping = false;  // pump's own cap
		if (user_stop_at >= 0 && t >= user_stop_at && t < user_stop_at + TICK_MS) s->pumping = false;
		presence_logic_input_t in = {
			.out = out_at(pulses, n, t),
			.enabled = s->enabled,
			.pump_any = s->pumping || s->other_pumping,
			.pump_ours = s->pumping,
		};
		presence_action_t a = presence_logic_step(&s->st, &s->p, t, &in);
		if (a == PRESENCE_ACTION_START) {
			s->pumping = true;
			s->run_until = t + s->p.max_session_ms;
			s->starts++;
			if (s->first_on < 0) s->first_on = t;
		} else if (a == PRESENCE_ACTION_STOP && s->pumping) {
			s->pumping = false;
			s->last_off = t;
		}
		if (s->pumping) s->on_ms += TICK_MS;
	}
}

// A drinking visit: a 3 s approach, then a 2.5 s pulse every `gap` ms.
static int visit(pulse_t *out, int64_t start, int64_t drink_ms, int64_t gap) {
	int n = 0;
	out[n++] = (pulse_t){start, start + 3000};
	for (int64_t t = start + 3000 + gap; t < start + 3000 + drink_ms; t += gap) {
		out[n++] = (pulse_t){t, t + 2500};
	}
	return n;
}

static void test_pass_by(void) {
	printf("pass-by: one 2.5 s pulse pours briefly, starting at once\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[] = {{20000, 22500}};
	sim_run(&s, p, 1, 0, 120000, -1);
	CHECK(s.starts == 1, "starts=%d", s.starts);
	CHECK(s.first_on - 20000 <= 600, "start delay %lld ms", (long long)(s.first_on - 20000));
	CHECK(s.on_ms >= 15000 && s.on_ms <= 35000, "poured %lld ms", (long long)s.on_ms);
}

static void test_glitch(void) {
	printf("glitch: a 200 ms spike never starts the pump\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[] = {{20000, 20200}};
	sim_run(&s, p, 1, 0, 60000, -1);
	CHECK(s.starts == 0, "starts=%d", s.starts);
}

static void test_drinking(void) {
	printf("drinking: pulses every 6 s keep one session going, then it ends\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[64];
	int n = visit(p, 20000, 90000, 6000);
	int64_t visit_end = p[n - 1].to;
	sim_run(&s, p, n, 0, 240000, -1);
	CHECK(s.starts == 1, "starts=%d", s.starts);
	CHECK(s.last_off > visit_end, "stopped %lld ms before the visit ended", (long long)(visit_end - s.last_off));
	CHECK(s.last_off - visit_end <= 45000, "kept pouring %lld ms after", (long long)(s.last_off - visit_end));
}

static void test_calm_cat(void) {
	printf("calm cat: pulses every 12 s still one session\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[64];
	int n = visit(p, 20000, 120000, 12000);
	sim_run(&s, p, n, 0, 260000, -1);
	CHECK(s.starts == 1, "starts=%d", s.starts);
}

static void test_two_visits(void) {
	printf("two visits 3 min apart: both get water\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[128];
	int n = visit(p, 20000, 40000, 5000);
	n += visit(p + n, 240000, 40000, 5000);
	sim_run(&s, p, n, 0, 400000, -1);
	CHECK(s.starts == 2, "starts=%d", s.starts);
}

static void test_confirm_two(void) {
	printf("confirm=2: a single short pulse is ignored, a second within 10 s starts\n");
	presence_logic_params_t pr = defaults();
	pr.th_on = presence_logic_th_on(2, 10000, pr.hold_ms);
	sim_t s;
	sim_init(&s, pr);
	pulse_t one[] = {{20000, 21000}};
	sim_run(&s, one, 1, 0, 60000, -1);
	CHECK(s.starts == 0, "single pulse: starts=%d", s.starts);

	sim_init(&s, pr);
	pulse_t two[] = {{20000, 21000}, {26000, 27000}};
	sim_run(&s, two, 2, 0, 60000, -1);
	CHECK(s.starts == 1, "two pulses 6 s apart: starts=%d", s.starts);
	CHECK(s.first_on >= 26000 && s.first_on <= 26600, "started at %lld", (long long)s.first_on);

	sim_init(&s, pr);
	pulse_t far[] = {{20000, 21000}, {35000, 36000}};
	sim_run(&s, far, 2, 0, 60000, -1);
	CHECK(s.starts == 0, "two pulses 15 s apart: starts=%d", s.starts);
}

static void test_stuck_high(void) {
	printf("stuck HIGH: one capped session, fault, nothing more until OUT drops\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[] = {{10000, 1210000}, {1300000, 1302500}};
	sim_run(&s, p, 1, 0, 1210000, -1);
	CHECK(s.starts == 1, "starts=%d", s.starts);
	CHECK(s.on_ms <= 300000 + TICK_MS, "poured %lld ms", (long long)s.on_ms);
	CHECK(s.st.fault, "fault not raised");
	sim_run(&s, p, 2, 1210000, 1400000, -1);
	CHECK(!s.st.fault, "fault not cleared");
	CHECK(s.starts == 2, "after recovery starts=%d", s.starts);
}

static void test_user_stop(void) {
	printf("app stop: no restart while the cat stays, normal again after it leaves\n");
	sim_t s;
	sim_init(&s, defaults());
	pulse_t p[128];
	int n = visit(p, 20000, 120000, 4000);
	n += visit(p + n, 400000, 20000, 4000);
	sim_run(&s, p, n, 0, 300000, 30000);
	CHECK(s.starts == 1, "restarted after stop: starts=%d", s.starts);
	sim_run(&s, p, n, 300000, 500000, -1);
	CHECK(s.starts == 2, "next visit: starts=%d", s.starts);
}

static void test_never_leaves(void) {
	printf("cat that never leaves: one max-length session, no restart\n");
	sim_t s;
	sim_init(&s, defaults());
	static pulse_t p[2000];
	int n = visit(p, 1000, 3600000, 4000);
	sim_run(&s, p, n, 0, 3600000, -1);
	CHECK(s.starts == 1, "starts=%d", s.starts);
	CHECK(s.on_ms <= 300000 + TICK_MS, "poured %lld ms", (long long)s.on_ms);
}

static void test_budget(void) {
	printf("budget: 4-min visits every 6 min get at most budget_ms per hour\n");
	presence_logic_params_t pr = defaults();
	pr.budget_ms = 600000;
	sim_t s;
	sim_init(&s, pr);
	static pulse_t p[2000];
	int n = 0;
	for (int64_t t = 1000; t < 3600000; t += 360000) {
		n += visit(p + n, t, 240000, 4000);
	}
	sim_run(&s, p, n, 0, 3600000, -1);
	CHECK(s.on_ms <= 600000 + TICK_MS, "poured %lld ms", (long long)s.on_ms);
	CHECK(s.on_ms >= 590000, "poured only %lld ms", (long long)s.on_ms);
}

static void test_disable(void) {
	printf("mode off: no start, and a running session stops\n");
	sim_t s;
	sim_init(&s, defaults());
	s.enabled = false;
	pulse_t p[] = {{20000, 22500}};
	sim_run(&s, p, 1, 0, 30000, -1);
	CHECK(s.starts == 0, "disabled: starts=%d", s.starts);

	sim_init(&s, defaults());
	pulse_t q[] = {{20000, 40000}};
	sim_run(&s, q, 1, 0, 25000, -1);
	s.enabled = false;
	sim_run(&s, q, 1, 25000, 26000, -1);
	CHECK(!s.pumping, "still pumping after mode off");
}

static void test_other_source(void) {
	printf("schedule running: presence does not queue a run behind it\n");
	sim_t s;
	sim_init(&s, defaults());
	s.other_pumping = true;
	pulse_t p[] = {{20000, 22500}};
	sim_run(&s, p, 1, 0, 60000, -1);
	CHECK(s.starts == 0, "starts=%d", s.starts);
}

static void test_rest(void) {
	printf("rest: a pulse right after a session waits out rest_ms\n");
	presence_logic_params_t pr = defaults();
	pr.rest_ms = 60000;
	sim_t s;
	sim_init(&s, pr);
	pulse_t p[] = {{20000, 21000}, {60000, 61000}, {120000, 121000}};
	sim_run(&s, p, 3, 0, 160000, -1);
	// First session ends ~38 s; the 60 s pulse falls inside the rest.
	CHECK(s.starts == 2, "starts=%d", s.starts);
}

int main(void) {
	test_pass_by();
	test_glitch();
	test_drinking();
	test_calm_cat();
	test_two_visits();
	test_confirm_two();
	test_stuck_high();
	test_user_stop();
	test_never_leaves();
	test_budget();
	test_disable();
	test_other_source();
	test_rest();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("all passed\n");
	return EXIT_SUCCESS;
}
