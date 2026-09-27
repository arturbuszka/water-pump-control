#include "presence.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "pump.h"
#include "pump_config.h"

static const char *TAG = "presence";

typedef enum {
	PRESENCE_STATE_ARMED,
	PRESENCE_STATE_COOLDOWN,
} presence_state_t;

static volatile bool s_last_detected;

static bool read_presence_raw(void) {
	int level = gpio_get_level(PRESENCE_GPIO);
	bool present = PRESENCE_ACTIVE_HIGH ? (level != 0) : (level == 0);
	s_last_detected = present;
	return present;
}

bool presence_is_detected(void) { return s_last_detected; }

// Blocks for up to (PRESENCE_DEBOUNCE_SAMPLES - 1) * PRESENCE_DEBOUNCE_MS,
// re-sampling PRESENCE_GPIO. Returns true only if every sample agreed the
// target is present - a single noisy sample at the edge of range does not
// count as a detection.
static bool debounce_confirms_presence(void) {
	for (int i = 0; i < PRESENCE_DEBOUNCE_SAMPLES; i++) {
		if (!read_presence_raw()) {
			return false;
		}
		if (i < PRESENCE_DEBOUNCE_SAMPLES - 1) {
			vTaskDelay(pdMS_TO_TICKS(PRESENCE_DEBOUNCE_MS));
		}
	}
	return true;
}

static void presence_task(void *arg) {
	(void)arg;
	presence_state_t state = PRESENCE_STATE_ARMED;
	int64_t cooldown_until_us = 0;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(PRESENCE_POLL_MS));

		// Always sample so the status endpoint has a live reading regardless
		// of mode or cooldown; the trigger logic below re-reads through the
		// debounce when it actually needs to act on it.
		bool raw_present = read_presence_raw();

		pump_config_t cfg;
		pump_get_config(&cfg);
		if (cfg.mode != PUMP_MODE_PRESENCE) {
			// Mode can change back at any time from the app; re-arm so a fresh
			// switch into presence mode doesn't inherit a stale cooldown.
			state = PRESENCE_STATE_ARMED;
			continue;
		}

		if (state == PRESENCE_STATE_COOLDOWN) {
			if (esp_timer_get_time() >= cooldown_until_us) {
				state = PRESENCE_STATE_ARMED;
				ESP_LOGI(TAG, "cooldown elapsed, re-armed");
			}
			continue;
		}

		if (!raw_present) {
			continue;
		}
		if (!debounce_confirms_presence()) {
			continue;  // noise at the edge of range
		}

		ESP_LOGI(TAG, "presence confirmed");
		esp_err_t err = pump_request(PUMP_SOURCE_PRESENCE, 0);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "presence-triggered request rejected: %s", esp_err_to_name(err));
		}
		state = PRESENCE_STATE_COOLDOWN;
		cooldown_until_us = esp_timer_get_time() + (int64_t)cfg.presence_cooldown_seconds * 1000000;
	}
}

esp_err_t presence_init(void) {
	gpio_config_t gpio_cfg = {
		.pin_bit_mask = 1ULL << PRESENCE_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	esp_err_t err = gpio_config(&gpio_cfg);
	if (err != ESP_OK) {
		return err;
	}

	if (xTaskCreate(presence_task, "presence", 3072, NULL, 4, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	ESP_LOGI(TAG, "GPIO %d, active %s, polling every %d ms", PRESENCE_GPIO,
	         PRESENCE_ACTIVE_HIGH ? "HIGH" : "LOW", PRESENCE_POLL_MS);
	return ESP_OK;
}
