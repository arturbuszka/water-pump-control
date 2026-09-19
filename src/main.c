#include <stdio.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "pump";

// Survives deep sleep: RTC memory keeps its contents while the CPU is off.
// Plain globals are re-initialised on every wake, because a deep-sleep wake is a
// fresh boot, not a resumed program.
static RTC_DATA_ATTR uint32_t cycle_count = 0;

// Configure the relay pin and park it in the OFF state.
static void relay_init(void) {
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << RELAY_GPIO,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&cfg));
	ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, RELAY_LEVEL_OFF));
}

static void pump_run_once(void) {
	ESP_LOGI(TAG, "pump ON for %d s", PUMP_ON_SECONDS_EFFECTIVE);
	ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, RELAY_LEVEL_ON));

	vTaskDelay(pdMS_TO_TICKS(PUMP_ON_SECONDS_EFFECTIVE * 1000));

	ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, RELAY_LEVEL_OFF));
	ESP_LOGI(TAG, "pump OFF");
}

// Without this the pin floats while the CPU is off, and a floating IN can switch
// the relay on mid-sleep - the pump would run unattended until the battery died.
//
// On the classic ESP32 the latch lives in the RTC domain, so the pin must be an
// RTC-capable GPIO (GPIO27 is) and that domain has to stay powered through sleep -
// otherwise the hold is lost the moment the chip powers down.
static void hold_relay_through_sleep(void) {
	ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, RELAY_LEVEL_OFF));
	ESP_ERROR_CHECK(rtc_gpio_hold_en(RELAY_GPIO));
	ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
}

static void enter_deep_sleep(void) {
	const uint64_t sleep_us = (uint64_t)PUMP_INTERVAL_SECONDS * 1000000ULL;

	hold_relay_through_sleep();

	ESP_LOGI(TAG, "sleeping %d s until next cycle", PUMP_INTERVAL_SECONDS);
	// Let the USB console flush before the peripheral powers down, otherwise the
	// last line never reaches the monitor.
	vTaskDelay(pdMS_TO_TICKS(100));

	ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(sleep_us));
	esp_deep_sleep_start();  // does not return
}

void app_main(void) {
	esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
	bool woke_from_timer = (cause == ESP_SLEEP_WAKEUP_TIMER);

	// The hold survives the wake, so release it before driving the pin again.
	rtc_gpio_hold_dis(RELAY_GPIO);
	relay_init();

	ESP_LOGI(TAG, "=== water pump controller (ESP32-WROOM-32 DevKit) ===");
	ESP_LOGI(TAG, "GPIO %d, active %s, run %d s, interval %d s",
	         RELAY_GPIO, RELAY_ACTIVE_LOW ? "LOW" : "HIGH",
	         PUMP_ON_SECONDS_EFFECTIVE, PUMP_INTERVAL_SECONDS);

#if PUMP_ON_SECONDS > PUMP_MAX_ON_SECONDS
	ESP_LOGW(TAG, "PUMP_ON_SECONDS (%d) clamped to safety cap %d s",
	         PUMP_ON_SECONDS, PUMP_MAX_ON_SECONDS);
#endif

	if (woke_from_timer) {
		cycle_count++;
		ESP_LOGI(TAG, "woke from timer, cycle #%lu", (unsigned long)cycle_count);
		pump_run_once();
	} else {
		// Power-on, reset or flash: a cold start.
		cycle_count = 0;
		ESP_LOGI(TAG, "cold boot (cause %d)", (int)cause);
		if (RUN_ON_FIRST_BOOT) {
			pump_run_once();
		} else {
			ESP_LOGI(TAG, "skipping first watering (RUN_ON_FIRST_BOOT = 0)");
		}
	}

	enter_deep_sleep();
}
