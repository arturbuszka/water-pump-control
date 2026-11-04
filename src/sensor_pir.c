#include "presence_sensor.h"

#include "driver/gpio.h"

#include "config.h"

// A PIR module drives OUT itself, but not every one drives it hard while LOW,
// and a long lead to the bowl picks up noise. The pull-down keeps an idle or
// unplugged sensor reading "nobody there" rather than floating into a pour.
static esp_err_t pir_init(void) {
	gpio_config_t gpio_cfg = {
		.pin_bit_mask = 1ULL << PRESENCE_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = PRESENCE_ACTIVE_HIGH ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	return gpio_config(&gpio_cfg);
}

const presence_sensor_t presence_sensor_pir = {
	.type = PRESENCE_SENSOR_PIR,
	.warmup_ms = PIR_WARMUP_SECONDS * 1000,
	.has_tuning = false,
	.init = pir_init,
	.apply_config = NULL,
	.responding = NULL,
};
