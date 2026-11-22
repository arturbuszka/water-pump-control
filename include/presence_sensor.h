#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "pump_config.h"

// What a presence sensor driver provides. Every supported sensor reports
// through the same OUT pin (PRESENCE_GPIO), so presence.c reads that itself;
// a driver only covers what differs around it - pin setup, warm-up, tuning,
// and whether it can tell that it is alive.
typedef struct {
	presence_sensor_type_t type;
	// After init(), how long OUT is not trusted for triggering.
	uint32_t warmup_ms;
	// Whether the presence_max_gate / _sensitivity / _no_one_seconds fields
	// mean anything to this sensor. The API rejects them otherwise.
	bool has_tuning;
	// Sets up PRESENCE_GPIO and whatever else the sensor needs.
	esp_err_t (*init)(void);
	// Pushes the tuning fields into the sensor. NULL if it has none.
	esp_err_t (*apply_config)(const pump_config_t *cfg);
	// Whether the sensor answered the last time it was talked to. NULL if
	// there is no way to tell - a quiet PIR looks the same as a missing one.
	bool (*responding)(void);
} presence_sensor_t;

extern const presence_sensor_t presence_sensor_pir;
extern const presence_sensor_t presence_sensor_ld2410;

// Driver for a sensor type; never NULL for a type pump_config_validate()
// accepts.
const presence_sensor_t *presence_sensor_for(presence_sensor_type_t type);
