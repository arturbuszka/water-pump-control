#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Polls the LD2410C's OUT pin (PRESENCE_GPIO) and requests a pump run through
// pump_request() when it sees a debounced, cooldown-cleared detection while
// the live config is in PUMP_MODE_PRESENCE. A no-op the rest of the time -
// pump_apply_config() already keeps the pump layer's own mode check, this
// task just also skips polling work when it isn't the active mode.
//
// Must be called after pump_init(), since a detection calls pump_request().
esp_err_t presence_init(void);

// True if the sensor currently reads "present" (debounced), regardless of
// mode or cooldown - lets the status endpoint show a live reading even when
// presence mode isn't the active trigger.
bool presence_is_detected(void);
