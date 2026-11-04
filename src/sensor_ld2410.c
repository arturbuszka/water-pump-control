#include "presence_sensor.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "config.h"
#include "ld2410_uart.h"

static const char *TAG = "ld2410";

// Result of the last tuning push - the only time the radar talks back.
static bool s_responding;

static esp_err_t ld2410_init(void) {
	// The radar drives OUT push-pull, so no pulls.
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

	// Best-effort: without the UART the radar still works on its own stored
	// tuning, and OUT is all presence mode needs.
	err = ld2410_uart_init();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "UART init failed (%s), sensor tuning unavailable", esp_err_to_name(err));
	}
	return ESP_OK;
}

static esp_err_t ld2410_apply_config(const pump_config_t *cfg) {
	esp_err_t err = ld2410_uart_apply_config(cfg);
	s_responding = (err == ESP_OK);
	return err;
}

static bool ld2410_responding(void) { return s_responding; }

const presence_sensor_t presence_sensor_ld2410 = {
	.type = PRESENCE_SENSOR_LD2410,
	.warmup_ms = 0,
	.has_tuning = true,
	.init = ld2410_init,
	.apply_config = ld2410_apply_config,
	.responding = ld2410_responding,
};
