#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "presence_sensor.h"
#include "pump_config.h"

// Brings up the sensor named by cfg->presence_sensor, pushes its tuning, then
// polls its OUT pin (PRESENCE_GPIO) every PRESENCE_POLL_MS. In
// PUMP_MODE_PRESENCE the samples drive presence_logic.c, which starts a
// presence run when the motion score crosses its threshold and stops it once
// the score has decayed - see presence_logic.h for the model. In other modes,
// and during quiet hours, the pin is still sampled and logged, but never acted
// on.
//
// The sensor type is fixed from here until the next boot. Must be called after
// pump_init(), since a detection calls pump_request().
esp_err_t presence_init(const pump_config_t *cfg);

// The driver chosen at boot.
const presence_sensor_t *presence_active_sensor(void);

// True while the sensor is still inside its post-boot warm-up, when OUT is
// logged and reported but never triggers a pour.
bool presence_warming_up(void);

// Pushes the tuning fields into the running sensor, if it has any and `cfg`
// still names it. Best-effort; the error is only for logging.
esp_err_t presence_apply_config(const pump_config_t *cfg);

// True if the sensor currently reads "present", regardless of mode - lets the
// status endpoint show a live reading even when presence mode isn't the
// active trigger.
bool presence_is_detected(void);

// How many OUT pulses presence_get_events() remembers. Kept in RAM only, so a
// reboot starts the log afresh - a collector is expected to poll it.
#define PRESENCE_EVENT_LOG_LEN 200

// One HIGH pulse on OUT, recorded in every mode so the raw signal can be
// studied before tuning how presence mode reacts to it. Resolution is
// PRESENCE_POLL_MS.
typedef struct {
	// Pulse number since boot, starting at 0.
	uint32_t seq;
	// esp_timer clock (microseconds since boot) at which OUT went HIGH.
	int64_t rise_us;
	// How long OUT stayed HIGH; 0 while `ongoing`.
	uint32_t high_ms;
	// OUT is still HIGH - this is the newest pulse.
	bool ongoing;
	// Whether the pump was running when the pulse started, to spot pulses the
	// relay or pump themselves may cause.
	bool pump_on;
} presence_event_t;

// Copies up to `max` recorded pulses into `out`, newest first. Returns how many.
size_t presence_get_events(presence_event_t *out, size_t max);

// What presence mode is doing right now, for the status endpoint.
typedef struct {
	float score;             // the motion score x
	bool session;            // a presence session is pouring
	bool fault;              // OUT stuck HIGH, sensor ignored
	uint32_t budget_used_s;  // presence pouring in the last hour
} presence_status_t;

void presence_get_status(presence_status_t *out);
