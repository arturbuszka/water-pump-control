#include <stdio.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "config.h"
#include "http_api.h"
#include "ld2410_uart.h"
#include "net.h"
#include "presence.h"
#include "pump.h"
#include "pump_config.h"

static const char *TAG = "main";

static void nvs_init(void) {
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// A partition left over from a different NVS layout. Wiping it costs the
		// stored settings, which then fall back to the config.h defaults.
		ESP_LOGW(TAG, "erasing NVS partition (%s)", esp_err_to_name(err));
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
}

void app_main(void) {
	ESP_LOGI(TAG, "=== water pump controller (ESP32-WROOM-32 DevKit) ===");

	nvs_init();

	pump_config_t cfg;
	pump_config_load(&cfg);

	// The pump comes up before the network on purpose: watering is the job, and
	// it has to keep running on schedule whether or not the WiFi ever appears.
	ESP_ERROR_CHECK(pump_init(&cfg));
	ESP_LOGI(TAG, "GPIO %d, active %s, mode %s", RELAY_GPIO, RELAY_ACTIVE_LOW ? "LOW" : "HIGH",
	         pump_mode_name(cfg.mode));

	ESP_ERROR_CHECK(presence_init());

	esp_err_t ld2410_err = ld2410_uart_init();
	if (ld2410_err == ESP_OK) {
		// Best-effort: a sensor that isn't wired up yet, or doesn't ack in
		// time, shouldn't stop the board from watering on schedule.
		ld2410_uart_apply_config(&cfg);
	} else {
		ESP_LOGW(TAG, "LD2410C UART init failed (%s), skipping sensor tuning",
		         esp_err_to_name(ld2410_err));
	}

	ESP_ERROR_CHECK(net_init());
	ESP_ERROR_CHECK(http_api_start());

	// app_main returns; the pump task, the schedule timer, the presence task
	// and the HTTP server carry on. There is no deep sleep any more - the
	// board stays reachable so a thirsty cat can get water outside the
	// schedule.
	ESP_LOGI(TAG, "init complete");
}
