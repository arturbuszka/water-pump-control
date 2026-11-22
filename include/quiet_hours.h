#pragma once

#include <stdbool.h>
#include <stdint.h>

// The quiet-hours arithmetic, kept free of ESP-IDF so it can be unit-tested on
// a PC (see test/test_quiet_hours.c).
//
// A window runs from start_minute up to, but not including, end_minute, both in
// local minutes since midnight. start > end crosses midnight: 23:00-06:00 is
// start 1380, end 360. start == end is ambiguous and rejected by
// pump_config_validate(), so it never reaches these functions.

#define QUIET_MINUTES_PER_DAY 1440u
#define QUIET_SECONDS_PER_DAY 86400u

// True if `minute` (0..1439) falls inside the window.
bool quiet_hours_contains(uint32_t start_minute, uint32_t end_minute, uint32_t minute);

// Seconds until the window ends, or 0 if `second_of_day` is outside it.
uint32_t quiet_hours_seconds_left(uint32_t start_minute, uint32_t end_minute,
                                  uint32_t second_of_day);

// When the schedule timer next fires outside the window, in seconds from now.
// `tick_in_s` is its next firing and it repeats every `interval_s`; ticks inside
// the window are the ones that get skipped. Returns -1 if every tick for weeks
// ahead lands inside - a daily interval whose phase sits inside the window.
int64_t quiet_hours_next_tick_s(uint32_t start_minute, uint32_t end_minute,
                                uint32_t second_of_day, uint32_t tick_in_s, uint32_t interval_s);
