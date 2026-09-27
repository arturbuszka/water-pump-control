#include "pump.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "pump";

typedef struct {
	pump_source_t source;
	uint32_t seconds;
} pump_request_t;

// Depth of 4: a couple of queued requests is plenty, and a full queue is a
// signal that something is spamming us rather than a case to buffer through.
#define PUMP_QUEUE_DEPTH 4

static QueueHandle_t s_requests;
static TaskHandle_t s_pump_task;
static SemaphoreHandle_t s_state_lock;
static esp_timer_handle_t s_schedule_timer;

static pump_config_t s_cfg;
static pump_state_t s_state;

// A stop applies to one particular run, identified by this counter, so a stop
// that arrives just before the next request cannot cancel it. s_run_id is the
// run currently in progress (0 when idle) and s_stop_run_id is the run a stop
// was asked for. Both are touched only under s_state_lock.
static uint32_t s_run_id;
static uint32_t s_stop_run_id;
static uint32_t s_next_run_id = 1;

// Ring buffer of finished runs, and when the periodic timer fires next. Both
// are touched only under s_state_lock.
static pump_history_entry_t s_history[PUMP_HISTORY_LEN];
static size_t s_history_next;
static size_t s_history_count;
static int64_t s_next_due_us;

static void history_push(const pump_history_entry_t *entry) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	s_history[s_history_next] = *entry;
	s_history_next = (s_history_next + 1) % PUMP_HISTORY_LEN;
	if (s_history_count < PUMP_HISTORY_LEN) {
		s_history_count++;
	}
	xSemaphoreGive(s_state_lock);
}

// True when the run that is currently executing has been told to stop.
static bool stop_pending(void) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	bool pending = (s_run_id != 0 && s_stop_run_id == s_run_id);
	xSemaphoreGive(s_state_lock);
	return pending;
}

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

static void state_set_idle(void) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	s_state.pumping = false;
	s_state.seconds_left = 0;
	s_state.seconds_total = 0;
	s_run_id = 0;
	xSemaphoreGive(s_state_lock);
}

// Marks a new run as started, giving it a fresh id so that only a stop aimed at
// this run can cut it short.
static void state_set_running(pump_source_t source, uint32_t seconds) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	s_state.pumping = true;
	s_state.seconds_left = seconds;
	s_state.seconds_total = seconds;
	s_state.source = source;
	s_state.cycle_count++;
	s_run_id = s_next_run_id++;
	if (s_next_run_id == 0) {
		s_next_run_id = 1;  // 0 means "idle", so skip it on wrap
	}
	xSemaphoreGive(s_state_lock);
}

static void state_tick(uint32_t seconds_left) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	s_state.seconds_left = seconds_left;
	xSemaphoreGive(s_state_lock);
}

// Runs the pump for `seconds`, waking every second so a stop request lands
// promptly instead of after the whole run. The relay is switched off on every
// exit path - this function must never return with the pump still energised.
static void pump_run(pump_source_t source, uint32_t seconds) {
	ESP_LOGI(TAG, "pump ON for %lu s (source %d)", (unsigned long)seconds, (int)source);
	state_set_running(source, seconds);
	ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, RELAY_LEVEL_ON));

	// Drive the loop off the clock rather than off a tick count: a notification
	// that is not a stop (a stale one, say) would otherwise skip its second and
	// cut the run short.
	const int64_t started_us = esp_timer_get_time();
	const TickType_t started = xTaskGetTickCount();
	const TickType_t duration = pdMS_TO_TICKS(seconds * 1000);
	bool stopped_early = false;
	for (;;) {
		TickType_t elapsed = xTaskGetTickCount() - started;
		if (elapsed >= duration) {
			break;
		}
		TickType_t remaining = duration - elapsed;
		TickType_t slice = remaining < pdMS_TO_TICKS(1000) ? remaining : pdMS_TO_TICKS(1000);

		// Returns early (non-zero) when pump_stop() notifies us.
		if (ulTaskNotifyTake(pdTRUE, slice) > 0 && stop_pending()) {
			ESP_LOGI(TAG, "run cut short after %lu s",
			         (unsigned long)((xTaskGetTickCount() - started) / configTICK_RATE_HZ));
			stopped_early = true;
			break;
		}
		elapsed = xTaskGetTickCount() - started;
		state_tick(elapsed >= duration ? 0 : (duration - elapsed) / configTICK_RATE_HZ);
	}

	ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, RELAY_LEVEL_OFF));
	state_set_idle();
	ESP_LOGI(TAG, "pump OFF");

	int64_t ran_us = esp_timer_get_time() - started_us;
	history_push(&(pump_history_entry_t){
		.started_us = started_us,
		.seconds_requested = seconds,
		.seconds_run = (uint32_t)((ran_us + 500000) / 1000000),
		.source = source,
		.stopped_early = stopped_early,
	});
}

static void pump_task(void *arg) {
	(void)arg;
	pump_request_t req;
	for (;;) {
		if (xQueueReceive(s_requests, &req, portMAX_DELAY) == pdTRUE) {
			// Clear any notification left over from a previous run, so it does
			// not cut the new one short. A stale stop cannot match the new run
			// id anyway, but the notification itself would still wake us early.
			ulTaskNotifyTake(pdTRUE, 0);
			pump_run(req.source, req.seconds);
		}
	}
}

static void schedule_timer_cb(void *arg) {
	(void)arg;
	pump_config_t cfg;
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	cfg = s_cfg;
	// The periodic timer fires again one interval from now, whether or not
	// this particular tick waters.
	s_next_due_us = esp_timer_get_time() + (int64_t)cfg.interval_seconds * 1000000;
	xSemaphoreGive(s_state_lock);
	if (!pump_schedule_active(&cfg)) {
		// On-demand and presence modes without the optional schedule keep the
		// timer armed but ignore it, so turning it on does not need the timer
		// rebuilt.
		return;
	}
	esp_err_t err = pump_request(PUMP_SOURCE_SCHEDULE, 0);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "scheduled cycle skipped: %s", esp_err_to_name(err));
	}
}

static void rearm_timer(uint32_t interval_seconds) {
	if (s_schedule_timer == NULL) {
		return;
	}
	esp_timer_stop(s_schedule_timer);  // fails harmlessly if not running
	ESP_ERROR_CHECK(esp_timer_start_periodic(s_schedule_timer,
	                                         (uint64_t)interval_seconds * 1000000ULL));
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	s_next_due_us = esp_timer_get_time() + (int64_t)interval_seconds * 1000000;
	xSemaphoreGive(s_state_lock);
}

esp_err_t pump_init(const pump_config_t *cfg) {
	s_cfg = *cfg;
	s_state = (pump_state_t){0};

	s_state_lock = xSemaphoreCreateMutex();
	s_requests = xQueueCreate(PUMP_QUEUE_DEPTH, sizeof(pump_request_t));
	if (s_state_lock == NULL || s_requests == NULL) {
		return ESP_ERR_NO_MEM;
	}

	relay_init();

	if (xTaskCreate(pump_task, "pump", 3072, NULL, 5, &s_pump_task) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}

	const esp_timer_create_args_t timer_args = {
		.callback = schedule_timer_cb,
		.name = "pump_schedule",
	};
	ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_schedule_timer));
	rearm_timer(s_cfg.interval_seconds);

	if (RUN_ON_FIRST_BOOT && pump_schedule_active(&s_cfg)) {
		ESP_LOGI(TAG, "first-boot watering");
		pump_request(PUMP_SOURCE_SCHEDULE, 0);
	}

	return ESP_OK;
}

esp_err_t pump_request(pump_source_t source, uint32_t seconds) {
	pump_config_t cfg;
	pump_get_config(&cfg);

	if (seconds == 0) {
		seconds = (source == PUMP_SOURCE_SCHEDULE) ? cfg.run_seconds : cfg.manual_run_seconds;
	}
	// The last line of defence: whatever the caller asked for, and whatever is
	// stored in NVS, the pump never runs longer than this.
	if (seconds == 0 || seconds > PUMP_MAX_ON_SECONDS) {
		ESP_LOGW(TAG, "rejected %lu s request (cap %d s)", (unsigned long)seconds,
		         PUMP_MAX_ON_SECONDS);
		return ESP_ERR_INVALID_ARG;
	}

	pump_request_t req = {.source = source, .seconds = seconds};
	if (xQueueSend(s_requests, &req, 0) != pdTRUE) {
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

void pump_stop(void) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	// Aims at the run in progress, if any. A stop arriving while idle is a
	// no-op rather than something that lies in wait for the next run.
	bool running = (s_run_id != 0);
	if (running) {
		s_stop_run_id = s_run_id;
	}
	xSemaphoreGive(s_state_lock);

	if (running && s_pump_task != NULL) {
		xTaskNotifyGive(s_pump_task);
	}
}

void pump_get_state(pump_state_t *out_state) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	*out_state = s_state;
	xSemaphoreGive(s_state_lock);
}

void pump_apply_config(const pump_config_t *cfg) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	uint32_t old_interval = s_cfg.interval_seconds;
	s_cfg = *cfg;
	xSemaphoreGive(s_state_lock);

	if (cfg->interval_seconds != old_interval) {
		rearm_timer(cfg->interval_seconds);
		ESP_LOGI(TAG, "schedule re-armed to %lus", (unsigned long)cfg->interval_seconds);
	}
}

void pump_get_config(pump_config_t *out_cfg) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	*out_cfg = s_cfg;
	xSemaphoreGive(s_state_lock);
}

size_t pump_get_history(pump_history_entry_t *out, size_t max) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	size_t n = s_history_count < max ? s_history_count : max;
	for (size_t i = 0; i < n; i++) {
		// s_history_next is one past the newest entry.
		size_t idx = (s_history_next + PUMP_HISTORY_LEN - 1 - i) % PUMP_HISTORY_LEN;
		out[i] = s_history[idx];
	}
	xSemaphoreGive(s_state_lock);
	return n;
}

int64_t pump_next_scheduled_us(void) {
	xSemaphoreTake(s_state_lock, portMAX_DELAY);
	int64_t due = pump_schedule_active(&s_cfg) ? s_next_due_us : -1;
	xSemaphoreGive(s_state_lock);
	return due;
}

const char *pump_source_name(pump_source_t source) {
	switch (source) {
		case PUMP_SOURCE_SCHEDULE: return "schedule";
		case PUMP_SOURCE_MANUAL: return "manual";
		case PUMP_SOURCE_PRESENCE: return "presence";
		default: return "unknown";
	}
}
