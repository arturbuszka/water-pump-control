#include "pump_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config.h"

static const char *TAG = "cfg";

// One namespace, one key per field. Keys are capped at 15 characters by NVS.
#define NVS_NAMESPACE "pump"
#define KEY_MODE "mode"
#define KEY_INTERVAL "interval_s"
#define KEY_RUN "run_s"
#define KEY_MANUAL "manual_s"

const char *pump_mode_name(pump_mode_t mode) {
	switch (mode) {
		case PUMP_MODE_INTERVAL: return "interval";
		case PUMP_MODE_ON_DEMAND: return "on_demand";
		case PUMP_MODE_PRESENCE: return "presence";
		default: return "unknown";
	}
}

bool pump_mode_from_name(const char *name, pump_mode_t *out_mode) {
	if (name == NULL || out_mode == NULL) {
		return false;
	}
	if (strcmp(name, "interval") == 0) {
		*out_mode = PUMP_MODE_INTERVAL;
	} else if (strcmp(name, "on_demand") == 0) {
		*out_mode = PUMP_MODE_ON_DEMAND;
	} else if (strcmp(name, "presence") == 0) {
		*out_mode = PUMP_MODE_PRESENCE;
	} else {
		return false;
	}
	return true;
}

bool pump_config_validate(const pump_config_t *cfg, char *reason, size_t reason_len) {
	if (cfg->mode != PUMP_MODE_INTERVAL && cfg->mode != PUMP_MODE_ON_DEMAND &&
	    cfg->mode != PUMP_MODE_PRESENCE) {
		snprintf(reason, reason_len, "unknown mode");
		return false;
	}
	// The run times are the only thing standing between a typo and a flooded
	// floor, so they are rejected outright rather than quietly clamped.
	if (cfg->run_seconds == 0 || cfg->run_seconds > PUMP_MAX_ON_SECONDS) {
		snprintf(reason, reason_len, "run_seconds must be 1..%d", PUMP_MAX_ON_SECONDS);
		return false;
	}
	if (cfg->manual_run_seconds == 0 || cfg->manual_run_seconds > PUMP_MAX_ON_SECONDS) {
		snprintf(reason, reason_len, "manual_run_seconds must be 1..%d", PUMP_MAX_ON_SECONDS);
		return false;
	}
	if (cfg->interval_seconds < PUMP_MIN_INTERVAL_SECONDS) {
		snprintf(reason, reason_len, "interval_seconds must be >= %d", PUMP_MIN_INTERVAL_SECONDS);
		return false;
	}
	// A cycle that outlasts its own interval would retrigger before it finished.
	if (cfg->run_seconds >= cfg->interval_seconds) {
		snprintf(reason, reason_len, "run_seconds must be shorter than interval_seconds");
		return false;
	}
	return true;
}

esp_err_t pump_config_load(pump_config_t *out_cfg) {
	// Factory defaults first, so every field is populated even if NVS is empty
	// or unreadable.
	*out_cfg = (pump_config_t){
		.mode = PUMP_MODE_INTERVAL,
		.interval_seconds = PUMP_INTERVAL_SECONDS,
		.run_seconds = PUMP_ON_SECONDS_EFFECTIVE,
		.manual_run_seconds = PUMP_MANUAL_ON_SECONDS,
	};

	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGI(TAG, "no stored config, using defaults from config.h");
		return ESP_OK;
	}
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "nvs_open failed (%s), using defaults", esp_err_to_name(err));
		return err;
	}

	uint8_t mode = (uint8_t)out_cfg->mode;
	nvs_get_u8(handle, KEY_MODE, &mode);
	out_cfg->mode = (pump_mode_t)mode;
	nvs_get_u32(handle, KEY_INTERVAL, &out_cfg->interval_seconds);
	nvs_get_u32(handle, KEY_RUN, &out_cfg->run_seconds);
	nvs_get_u32(handle, KEY_MANUAL, &out_cfg->manual_run_seconds);
	nvs_close(handle);

	// Stored values could predate a tightening of the limits, or come from a
	// half-finished write. Fall back rather than act on something unsafe.
	char reason[64];
	if (!pump_config_validate(out_cfg, reason, sizeof(reason))) {
		ESP_LOGW(TAG, "stored config rejected (%s), using defaults", reason);
		*out_cfg = (pump_config_t){
			.mode = PUMP_MODE_INTERVAL,
			.interval_seconds = PUMP_INTERVAL_SECONDS,
			.run_seconds = PUMP_ON_SECONDS_EFFECTIVE,
			.manual_run_seconds = PUMP_MANUAL_ON_SECONDS,
		};
		return ESP_OK;
	}

	ESP_LOGI(TAG, "loaded: mode=%s interval=%lus run=%lus manual=%lus",
	         pump_mode_name(out_cfg->mode), (unsigned long)out_cfg->interval_seconds,
	         (unsigned long)out_cfg->run_seconds, (unsigned long)out_cfg->manual_run_seconds);
	return ESP_OK;
}

esp_err_t pump_config_save(const pump_config_t *cfg) {
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open for write failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_u8(handle, KEY_MODE, (uint8_t)cfg->mode);
	if (err == ESP_OK) err = nvs_set_u32(handle, KEY_INTERVAL, cfg->interval_seconds);
	if (err == ESP_OK) err = nvs_set_u32(handle, KEY_RUN, cfg->run_seconds);
	if (err == ESP_OK) err = nvs_set_u32(handle, KEY_MANUAL, cfg->manual_run_seconds);
	if (err == ESP_OK) err = nvs_commit(handle);

	nvs_close(handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "config saved");
	}
	return err;
}
