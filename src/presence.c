#include "presence.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "presence_logic.h"
#include "presence_sensor.h"
#include "pump.h"
#include "pump_config.h"

static const char *TAG = "presence";

static volatile bool s_last_detected;

// Copy of the logic's state for the status endpoint, refreshed every poll.
static presence_status_t s_status;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;

// Chosen once at boot from the stored config; see presence_init().
static const presence_sensor_t *s_sensor;
// esp_timer time until which OUT is not trusted for triggering.
static int64_t s_warmup_until_us;

// Ring buffer of OUT pulses, oldest overwritten first. s_event_seq counts every
// pulse since boot, so it doubles as the index of the next slot and lets a
// collector polling the endpoint drop what it has already seen.
static presence_event_t s_events[PRESENCE_EVENT_LOG_LEN];
static uint32_t s_event_seq;
static SemaphoreHandle_t s_events_lock;

// Called on every edge read_presence_raw() sees. A rise opens a new entry; a
// fall closes the newest one, which is always the pulse that is still high.
static void record_edge(bool rising) {
	int64_t now_us = esp_timer_get_time();
	pump_state_t pump;
	if (rising) {
		pump_get_state(&pump);
	}
	xSemaphoreTake(s_events_lock, portMAX_DELAY);
	if (rising) {
		s_events[s_event_seq % PRESENCE_EVENT_LOG_LEN] = (presence_event_t){
			.seq = s_event_seq,
			.rise_us = now_us,
			.high_ms = 0,
			.ongoing = true,
			.pump_on = pump.pumping,
		};
		s_event_seq++;
	} else if (s_event_seq > 0) {
		presence_event_t *ev = &s_events[(s_event_seq - 1) % PRESENCE_EVENT_LOG_LEN];
		ev->high_ms = (uint32_t)((now_us - ev->rise_us) / 1000);
		ev->ongoing = false;
	}
	xSemaphoreGive(s_events_lock);
}

static bool read_presence_raw(void) {
	int level = gpio_get_level(PRESENCE_GPIO);
	bool present = PRESENCE_ACTIVE_HIGH ? (level != 0) : (level == 0);
	if (present != s_last_detected) {
		record_edge(present);
	}
	s_last_detected = present;
	return present;
}

bool presence_is_detected(void) { return s_last_detected; }

const presence_sensor_t *presence_sensor_for(presence_sensor_type_t type) {
	switch (type) {
		case PRESENCE_SENSOR_LD2410: return &presence_sensor_ld2410;
		case PRESENCE_SENSOR_PIR:
		default: return &presence_sensor_pir;
	}
}

const presence_sensor_t *presence_active_sensor(void) { return s_sensor; }

bool presence_warming_up(void) { return esp_timer_get_time() < s_warmup_until_us; }

esp_err_t presence_apply_config(const pump_config_t *cfg) {
	// A config naming a different sensor only takes effect after a restart;
	// pushing its tuning into the one actually wired would be meaningless.
	if (s_sensor->apply_config == NULL || cfg->presence_sensor != s_sensor->type) {
		return ESP_OK;
	}
	return s_sensor->apply_config(cfg);
}

size_t presence_get_events(presence_event_t *out, size_t max) {
	xSemaphoreTake(s_events_lock, portMAX_DELAY);
	uint32_t stored = s_event_seq < PRESENCE_EVENT_LOG_LEN ? s_event_seq : PRESENCE_EVENT_LOG_LEN;
	size_t n = stored < max ? stored : max;
	for (size_t i = 0; i < n; i++) {
		out[i] = s_events[(s_event_seq - 1 - i) % PRESENCE_EVENT_LOG_LEN];
	}
	xSemaphoreGive(s_events_lock);
	return n;
}

void presence_get_status(presence_status_t *out) {
	taskENTER_CRITICAL(&s_status_mux);
	*out = s_status;
	taskEXIT_CRITICAL(&s_status_mux);
}

static presence_logic_params_t params_from(const pump_config_t *cfg) {
	uint32_t hold_ms = cfg->presence_hold_seconds * 1000;
	return (presence_logic_params_t){
		.hold_ms = hold_ms,
		.th_on = presence_logic_th_on(cfg->presence_confirm_pulses,
		                              cfg->presence_confirm_window_seconds * 1000, hold_ms),
		.th_off = PRESENCE_TH_OFF,
		.x_max = PRESENCE_X_MAX,
		.min_pulse_ms = PRESENCE_MIN_PULSE_MS,
		.reemit_ms = PRESENCE_REEMIT_MS,
		.blank_ms = PRESENCE_BLANK_MS,
		.rest_ms = cfg->presence_rest_seconds * 1000,
		.max_session_ms = cfg->presence_max_session_seconds * 1000,
		.budget_ms = cfg->presence_hourly_budget_seconds * 1000,
		.stuck_ms = PRESENCE_STUCK_SECONDS * 1000,
	};
}

static void presence_task(void *arg) {
	(void)arg;
	presence_logic_t logic;
	presence_logic_init(&logic, esp_timer_get_time() / 1000);
	bool was_fault = false;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(PRESENCE_POLL_MS));

		// Sampled in every mode, so the status endpoint and the pulse log
		// always have a live reading.
		bool present = read_presence_raw();

		pump_config_t cfg;
		pump_get_config(&cfg);
		pump_state_t pump;
		pump_get_state(&pump);
		bool ours = pump.pumping && pump.source == PUMP_SOURCE_PRESENCE;

		presence_logic_params_t params = params_from(&cfg);
		presence_logic_input_t in = {
			.out = present,
			// Mode can change at any time from the app; leaving presence mode
			// resets the score so coming back doesn't inherit a stale one. Quiet
			// hours work the same way: a session in progress ends, and the morning
			// starts from a clean score.
			.enabled = cfg.mode == PUMP_MODE_PRESENCE && !presence_warming_up() &&
			           !pump_quiet_active(&cfg),
			.pump_any = pump.pumping,
			.pump_ours = ours,
		};
		int64_t now_ms = esp_timer_get_time() / 1000;
		presence_action_t action = presence_logic_step(&logic, &params, now_ms, &in);

		if (action == PRESENCE_ACTION_START) {
			ESP_LOGI(TAG, "session start (score %.2f)", logic.x);
			// The logic ends the session itself; the duration is only the cap.
			esp_err_t err = pump_request(PUMP_SOURCE_PRESENCE, cfg.presence_max_session_seconds);
			if (err != ESP_OK) {
				ESP_LOGW(TAG, "presence-triggered request rejected: %s", esp_err_to_name(err));
			}
		} else if (action == PRESENCE_ACTION_STOP) {
			ESP_LOGI(TAG, "session end (score %.2f%s)", logic.x, logic.fault ? ", sensor stuck" : "");
			// Only ever cut short our own run - never a schedule cycle that
			// happens to hold the pump now.
			if (ours) {
				pump_stop();
			}
		}
		if (logic.fault != was_fault) {
			was_fault = logic.fault;
			if (logic.fault) {
				ESP_LOGW(TAG, "OUT HIGH for %d s without a break - ignoring the sensor until it drops",
				         PRESENCE_STUCK_SECONDS);
			} else {
				ESP_LOGI(TAG, "OUT dropped, sensor trusted again");
			}
		}

		presence_status_t st = {
			.score = logic.x,
			.session = logic.session,
			.fault = logic.fault,
			.budget_used_s = presence_logic_budget_used_ms(&logic, now_ms) / 1000,
		};
		taskENTER_CRITICAL(&s_status_mux);
		s_status = st;
		taskEXIT_CRITICAL(&s_status_mux);
	}
}

esp_err_t presence_init(const pump_config_t *cfg) {
	s_events_lock = xSemaphoreCreateMutex();
	if (s_events_lock == NULL) {
		return ESP_ERR_NO_MEM;
	}

	s_sensor = presence_sensor_for(cfg->presence_sensor);
	esp_err_t err = s_sensor->init();
	if (err != ESP_OK) {
		return err;
	}
	s_warmup_until_us = esp_timer_get_time() + (int64_t)s_sensor->warmup_ms * 1000;
	// Best-effort: a sensor that isn't wired up yet, or doesn't ack in time,
	// shouldn't stop the board from watering on schedule.
	presence_apply_config(cfg);

	if (xTaskCreate(presence_task, "presence", 3072, NULL, 4, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	ESP_LOGI(TAG, "%s on GPIO %d, active %s, polling every %d ms, warm-up %lu ms",
	         presence_sensor_type_name(s_sensor->type), PRESENCE_GPIO,
	         PRESENCE_ACTIVE_HIGH ? "HIGH" : "LOW", PRESENCE_POLL_MS,
	         (unsigned long)s_sensor->warmup_ms);
	return ESP_OK;
}
