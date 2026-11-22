#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// The local time of day, for quiet hours. Everything else on the board runs off
// esp_timer, which an SNTP step does not touch - so the schedule, the presence
// logic and the history carry on undisturbed when the clock is set.

// Sets the time zone. Call before anything asks for the time.
void wall_clock_init(void);

// Starts SNTP, syncing on every new IP address and periodically after that.
// Call after net_init(). A failure only means the time stays unknown.
esp_err_t wall_clock_start_sync(void);

// Current local seconds since midnight. Returns false while the time is unknown:
// after a power loss, until the first sync.
//
// This asks the clock itself rather than remembering that a sync happened, so
// the answer stays right across anything the RTC survives, such as a soft reset.
bool wall_clock_second_of_day(uint32_t *out);
