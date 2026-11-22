#include "quiet_hours.h"

// Each pass jumps a whole window ahead, so this bounds the search to about a
// month of windows rather than a month of ticks.
#define NEXT_TICK_MAX_WINDOWS 32

bool quiet_hours_contains(uint32_t start_minute, uint32_t end_minute, uint32_t minute) {
	if (start_minute < end_minute) {
		return minute >= start_minute && minute < end_minute;
	}
	return minute >= start_minute || minute < end_minute;
}

uint32_t quiet_hours_seconds_left(uint32_t start_minute, uint32_t end_minute,
                                  uint32_t second_of_day) {
	if (!quiet_hours_contains(start_minute, end_minute, second_of_day / 60)) {
		return 0;
	}
	uint32_t end_second = end_minute * 60;
	if (end_second > second_of_day) {
		return end_second - second_of_day;
	}
	return end_second + QUIET_SECONDS_PER_DAY - second_of_day;
}

int64_t quiet_hours_next_tick_s(uint32_t start_minute, uint32_t end_minute,
                                uint32_t second_of_day, uint32_t tick_in_s, uint32_t interval_s) {
	int64_t tick = tick_in_s;
	for (int i = 0; i < NEXT_TICK_MAX_WINDOWS; i++) {
		uint32_t tick_second = (uint32_t)(((int64_t)second_of_day + tick) % QUIET_SECONDS_PER_DAY);
		uint32_t left = quiet_hours_seconds_left(start_minute, end_minute, tick_second);
		if (left == 0) {
			return tick;
		}
		// The first tick at or after the end of this window.
		tick += (int64_t)(((uint64_t)left + interval_s - 1) / interval_s) * interval_s;
	}
	return -1;
}
