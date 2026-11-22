// Host-side tests for quiet_hours.c - no ESP32 needed:
//
//     gcc -std=c11 -Wall -Wextra -Iinclude test/test_quiet_hours.c src/quiet_hours.c
//         -o /tmp/test_quiet_hours && /tmp/test_quiet_hours

#include <stdio.h>

#include "quiet_hours.h"

static int failures;

#define CHECK_EQ(actual, expected)                                                          \
	do {                                                                                    \
		long long a_ = (long long)(actual), e_ = (long long)(expected);                     \
		if (a_ != e_) {                                                                     \
			printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #actual,   \
			       a_, e_);                                                                 \
			failures++;                                                                     \
		}                                                                                   \
	} while (0)

#define HM(h, m) ((h) * 60 + (m))
#define SEC(h, m, s) (HM(h, m) * 60 + (s))

static void test_window_crossing_midnight(void) {
	const uint32_t start = HM(23, 0), end = HM(6, 0);
	CHECK_EQ(quiet_hours_contains(start, end, HM(22, 59)), false);
	CHECK_EQ(quiet_hours_contains(start, end, HM(23, 0)), true);
	CHECK_EQ(quiet_hours_contains(start, end, HM(23, 59)), true);
	CHECK_EQ(quiet_hours_contains(start, end, HM(0, 0)), true);
	CHECK_EQ(quiet_hours_contains(start, end, HM(5, 59)), true);
	CHECK_EQ(quiet_hours_contains(start, end, HM(6, 0)), false);
	CHECK_EQ(quiet_hours_contains(start, end, HM(12, 0)), false);
}

static void test_window_within_a_day(void) {
	const uint32_t start = HM(9, 30), end = HM(17, 0);
	CHECK_EQ(quiet_hours_contains(start, end, HM(9, 29)), false);
	CHECK_EQ(quiet_hours_contains(start, end, HM(9, 30)), true);
	CHECK_EQ(quiet_hours_contains(start, end, HM(16, 59)), true);
	CHECK_EQ(quiet_hours_contains(start, end, HM(17, 0)), false);
	CHECK_EQ(quiet_hours_contains(start, end, HM(0, 0)), false);
}

static void test_window_ending_at_midnight(void) {
	const uint32_t start = HM(22, 0), end = 0;
	CHECK_EQ(quiet_hours_contains(start, end, HM(21, 59)), false);
	CHECK_EQ(quiet_hours_contains(start, end, HM(23, 59)), true);
	CHECK_EQ(quiet_hours_contains(start, end, 0), false);
	CHECK_EQ(quiet_hours_seconds_left(start, end, SEC(23, 59, 30)), 30);
}

static void test_seconds_left(void) {
	const uint32_t start = HM(23, 0), end = HM(6, 0);
	CHECK_EQ(quiet_hours_seconds_left(start, end, SEC(12, 0, 0)), 0);
	CHECK_EQ(quiet_hours_seconds_left(start, end, SEC(23, 0, 0)), 7 * 3600);
	CHECK_EQ(quiet_hours_seconds_left(start, end, SEC(0, 0, 0)), 6 * 3600);
	CHECK_EQ(quiet_hours_seconds_left(start, end, SEC(5, 59, 59)), 1);
	CHECK_EQ(quiet_hours_seconds_left(start, end, SEC(6, 0, 0)), 0);
}

static void test_next_tick(void) {
	const uint32_t start = HM(23, 0), end = HM(6, 0);
	const uint32_t hour = 3600;

	// Outside the window, the timer's own tick stands.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(12, 0, 0), 900, hour), 900);
	// A tick just before the window still runs.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(22, 30, 0), 1740, hour), 1740);

	// 22:50 now, ticks at 23:10, 00:10 ... 06:10: the 06:10 one is the first out.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(22, 50, 0), 20 * 60, hour),
	         7 * 3600 + 20 * 60);
	// From inside the window: 02:00 now, ticks at 02:15 ... 06:15.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(2, 0, 0), 15 * 60, hour),
	         4 * 3600 + 15 * 60);
	// A tick landing exactly on the end of the window runs: the end is exclusive.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(5, 0, 0), 3600, hour), 3600);

	// A long interval can jump past one window and land inside the next one, so
	// the search has to keep going until a tick really is outside.
	const uint32_t long_interval = 24 * 3600 + 30 * 60;
	int64_t tick = quiet_hours_next_tick_s(start, end, SEC(23, 0, 0), 30 * 60, long_interval);
	CHECK_EQ(tick > 0, true);
	CHECK_EQ(quiet_hours_contains(start, end, (uint32_t)(((SEC(23, 0, 0) + tick) % 86400) / 60)),
	         false);

	// Exactly daily, phase inside the window: it never comes out.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(1, 0, 0), 3600, 86400), -1);
	// The largest interval the API can store must not overflow.
	CHECK_EQ(quiet_hours_next_tick_s(start, end, SEC(1, 0, 0), 3600, 0xFFFFFFFFu) != 0, true);
}

int main(void) {
	test_window_crossing_midnight();
	test_window_within_a_day();
	test_window_ending_at_midnight();
	test_seconds_left();
	test_next_tick();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all quiet-hours tests passed\n");
	return 0;
}
