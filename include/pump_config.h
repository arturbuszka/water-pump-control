#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Runtime configuration, as opposed to the wiring constants in config.h.
//
// These values live in NVS so they survive a reboot and can be changed over the
// network without reflashing. config.h still supplies the factory defaults used
// the first time the board boots with an empty NVS partition.

typedef enum {
	// Water on a fixed schedule, every interval_seconds.
	PUMP_MODE_INTERVAL = 0,
	// Never water on a schedule - only when something asks for it.
	PUMP_MODE_ON_DEMAND = 1,
	// Water when the HLK-LD2410C radar sees a cat at the bowl. The driver is not
	// written yet, so this currently behaves like ON_DEMAND; it exists so the
	// mode can already be stored and reported.
	PUMP_MODE_PRESENCE = 2,
} pump_mode_t;

typedef struct {
	pump_mode_t mode;
	// Gap between scheduled cycles.
	uint32_t interval_seconds;
	// How long one scheduled cycle runs.
	uint32_t run_seconds;
	// How long a "pour now" request runs when it does not name its own duration.
	uint32_t manual_run_seconds;
} pump_config_t;

// Shortest interval we accept. Anything faster is a misconfiguration rather
// than a watering schedule, and would keep the pump running near-continuously.
#define PUMP_MIN_INTERVAL_SECONDS 60

// Loads the stored configuration, falling back to the config.h defaults for any
// key that is missing. Never fails in a way that leaves *out_cfg unusable.
esp_err_t pump_config_load(pump_config_t *out_cfg);

// Persists the configuration. The caller is expected to have validated it.
esp_err_t pump_config_save(const pump_config_t *cfg);

// Checks a configuration against the safety cap and the minimum interval.
// On rejection, writes a short human-readable reason to `reason` - this is what
// the HTTP layer returns in its 400, so the caller learns which field was wrong.
bool pump_config_validate(const pump_config_t *cfg, char *reason, size_t reason_len);

// Mode <-> wire name, for the JSON API. pump_mode_from_name returns false on an
// unknown name rather than silently picking a default.
const char *pump_mode_name(pump_mode_t mode);
bool pump_mode_from_name(const char *name, pump_mode_t *out_mode);
