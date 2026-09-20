#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "pump_config.h"

// Everything that can ask for water. Recorded so the status endpoint and the
// logs can say why the pump is running.
typedef enum {
	PUMP_SOURCE_SCHEDULE = 0,
	PUMP_SOURCE_MANUAL = 1,
	PUMP_SOURCE_PRESENCE = 2,
} pump_source_t;

typedef struct {
	bool pumping;
	uint32_t seconds_left;
	uint32_t seconds_total;
	pump_source_t source;
	uint32_t cycle_count;
} pump_state_t;

// Starts the relay GPIO, the pump task and the schedule timer. Must be called
// after pump_config_load().
esp_err_t pump_init(const pump_config_t *cfg);

// Queues a run. `seconds` of 0 means "use the configured duration for this
// source". Returns ESP_ERR_INVALID_ARG if the duration exceeds the safety cap,
// or ESP_ERR_NO_MEM if the queue is full. Safe to call from any task.
esp_err_t pump_request(pump_source_t source, uint32_t seconds);

// Cuts a run short. A no-op if the pump is already idle.
void pump_stop(void);

// Snapshot of what the pump is doing right now.
void pump_get_state(pump_state_t *out_state);

// Swaps in a new configuration and re-arms the schedule timer. The caller is
// expected to have validated `cfg`.
void pump_apply_config(const pump_config_t *cfg);

// Current configuration, as the pump task sees it.
void pump_get_config(pump_config_t *out_cfg);
