#include "http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "net.h"
#include "presence.h"
#include "presence_sensor.h"
#include "pump.h"
#include "pump_config.h"

static const char *TAG = "api";

// Request bodies are a handful of integers - a full config from the app is
// ~400 bytes; anything much larger is not ours.
#define MAX_BODY_LEN 768

static esp_err_t send_json(httpd_req_t *req, int status, cJSON *root) {
	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (body == NULL) {
		return httpd_resp_send_500(req);
	}

	switch (status) {
		case 400: httpd_resp_set_status(req, "400 Bad Request"); break;
		case 409: httpd_resp_set_status(req, "409 Conflict"); break;
		case 503: httpd_resp_set_status(req, "503 Service Unavailable"); break;
		default: httpd_resp_set_status(req, "200 OK"); break;
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t err = httpd_resp_sendstr(req, body);
	free(body);
	return err;
}

static esp_err_t send_error(httpd_req_t *req, int status, const char *message) {
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", false);
	cJSON_AddStringToObject(root, "error", message);
	return send_json(req, status, root);
}

// Reads the whole body and parses it. Returns NULL after answering the client
// itself, so the caller just returns ESP_OK.
static cJSON *read_body_json(httpd_req_t *req, char *buf, size_t buf_len) {
	if (req->content_len == 0) {
		// An empty body is legal - it means "use the configured defaults".
		return cJSON_CreateObject();
	}
	if (req->content_len >= buf_len) {
		send_error(req, 400, "body too large");
		return NULL;
	}

	int received = httpd_req_recv(req, buf, req->content_len);
	if (received <= 0) {
		send_error(req, 400, "failed to read body");
		return NULL;
	}
	buf[received] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (root == NULL) {
		send_error(req, 400, "invalid JSON");
		return NULL;
	}
	return root;
}

// Pulls a non-negative integer field. A field that is present but not a number
// is an error rather than a missing value, so a typo does not pass silently.
static bool get_uint_field(const cJSON *root, const char *name, uint32_t *out, bool *found,
                           const char **bad_field) {
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
	if (item == NULL || cJSON_IsNull(item)) {
		*found = false;
		return true;
	}
	if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
		*bad_field = name;
		return false;
	}
	*out = (uint32_t)item->valuedouble;
	*found = true;
	return true;
}

// Pulls a boolean field. Like get_uint_field, a present field of the wrong type
// is an error rather than a missing value.
static bool get_bool_field(const cJSON *root, const char *name, bool *out, bool *found,
                           const char **bad_field) {
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
	if (item == NULL || cJSON_IsNull(item)) {
		*found = false;
		return true;
	}
	if (!cJSON_IsBool(item)) {
		*bad_field = name;
		return false;
	}
	*out = cJSON_IsTrue(item);
	*found = true;
	return true;
}

static void add_config_fields(cJSON *root, const pump_config_t *cfg) {
	cJSON_AddStringToObject(root, "mode", pump_mode_name(cfg->mode));
	cJSON_AddBoolToObject(root, "schedule_enabled", cfg->schedule_enabled);
	cJSON_AddNumberToObject(root, "interval_seconds", cfg->interval_seconds);
	cJSON_AddNumberToObject(root, "run_seconds", cfg->run_seconds);
	cJSON_AddNumberToObject(root, "manual_run_seconds", cfg->manual_run_seconds);
	cJSON_AddStringToObject(root, "presence_sensor", presence_sensor_type_name(cfg->presence_sensor));
	cJSON_AddNumberToObject(root, "presence_confirm_pulses", cfg->presence_confirm_pulses);
	cJSON_AddNumberToObject(root, "presence_confirm_window_seconds",
	                        cfg->presence_confirm_window_seconds);
	cJSON_AddNumberToObject(root, "presence_hold_seconds", cfg->presence_hold_seconds);
	cJSON_AddNumberToObject(root, "presence_max_session_seconds", cfg->presence_max_session_seconds);
	cJSON_AddNumberToObject(root, "presence_rest_seconds", cfg->presence_rest_seconds);
	cJSON_AddNumberToObject(root, "presence_hourly_budget_seconds",
	                        cfg->presence_hourly_budget_seconds);
	// Only the fields the configured sensor understands; the rest stay in NVS
	// for when it is swapped back.
	if (presence_sensor_for(cfg->presence_sensor)->has_tuning) {
		cJSON_AddNumberToObject(root, "presence_max_gate", cfg->presence_max_gate);
		cJSON_AddNumberToObject(root, "presence_sensitivity", cfg->presence_sensitivity);
		cJSON_AddNumberToObject(root, "presence_no_one_seconds", cfg->presence_no_one_seconds);
	}
}

// A sensor type saved over the API only takes over at the next boot.
static bool restart_required(const pump_config_t *cfg) {
	return cfg->presence_sensor != presence_active_sensor()->type;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
	pump_state_t state;
	pump_config_t cfg;
	pump_get_state(&state);
	pump_get_config(&cfg);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "pumping", state.pumping);
	cJSON_AddNumberToObject(root, "seconds_left", state.seconds_left);
	cJSON_AddNumberToObject(root, "seconds_total", state.seconds_total);
	cJSON_AddNumberToObject(root, "cycle_count", state.cycle_count);
	cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
	cJSON_AddNumberToObject(root, "wifi_rssi", net_rssi());
	cJSON_AddBoolToObject(root, "presence_detected", presence_is_detected());
	const presence_sensor_t *sensor = presence_active_sensor();
	cJSON_AddStringToObject(root, "presence_sensor_active", presence_sensor_type_name(sensor->type));
	if (sensor->responding == NULL) {
		cJSON_AddNullToObject(root, "presence_sensor_responding");
	} else {
		cJSON_AddBoolToObject(root, "presence_sensor_responding", sensor->responding());
	}
	cJSON_AddBoolToObject(root, "presence_warming_up", presence_warming_up());
	presence_status_t pres;
	presence_get_status(&pres);
	// Rounded: the score is for watching it rise and decay, not for maths.
	cJSON_AddNumberToObject(root, "presence_score", (double)((int)(pres.score * 100.0f + 0.5f)) / 100.0);
	cJSON_AddBoolToObject(root, "presence_session", pres.session);
	cJSON_AddBoolToObject(root, "presence_fault", pres.fault);
	cJSON_AddNumberToObject(root, "presence_budget_used_seconds", pres.budget_used_s);
	cJSON_AddBoolToObject(root, "restart_required", restart_required(&cfg));
	if (state.pumping) {
		cJSON_AddStringToObject(root, "source", pump_source_name(state.source));
	}
	// The board has no wall clock, so times go out relative to now and the app
	// turns them into clock times with its own.
	int64_t next_us = pump_next_scheduled_us();
	if (next_us < 0) {
		cJSON_AddNullToObject(root, "next_run_in_s");
	} else {
		int64_t in_us = next_us - esp_timer_get_time();
		cJSON_AddNumberToObject(root, "next_run_in_s", in_us > 0 ? (double)(in_us / 1000000) : 0);
	}
	add_config_fields(root, &cfg);
	return send_json(req, 200, root);
}

static esp_err_t history_get_handler(httpd_req_t *req) {
	pump_history_entry_t entries[PUMP_HISTORY_LEN];
	size_t n = pump_get_history(entries, PUMP_HISTORY_LEN);
	int64_t now_us = esp_timer_get_time();

	cJSON *root = cJSON_CreateObject();
	cJSON *runs = cJSON_AddArrayToObject(root, "runs");
	for (size_t i = 0; i < n; i++) {
		cJSON *run = cJSON_CreateObject();
		cJSON_AddStringToObject(run, "source", pump_source_name(entries[i].source));
		cJSON_AddNumberToObject(run, "started_ago_s",
		                        (double)((now_us - entries[i].started_us) / 1000000));
		cJSON_AddNumberToObject(run, "seconds", entries[i].seconds_run);
		cJSON_AddNumberToObject(run, "seconds_requested", entries[i].seconds_requested);
		cJSON_AddBoolToObject(run, "stopped_early", entries[i].stopped_early);
		cJSON_AddItemToArray(runs, run);
	}
	return send_json(req, 200, root);
}

// Raw PIR pulses for offline analysis. Times go out relative to now, like
// /api/history; `seq` plus `uptime_ms` let a poller de-duplicate across calls
// and notice a reboot (uptime going backwards resets seq).
static esp_err_t presence_events_get_handler(httpd_req_t *req) {
	// Too big for the httpd task's stack.
	presence_event_t *events = malloc(sizeof(presence_event_t) * PRESENCE_EVENT_LOG_LEN);
	if (events == NULL) {
		return send_error(req, 503, "out of memory");
	}
	size_t n = presence_get_events(events, PRESENCE_EVENT_LOG_LEN);
	int64_t now_us = esp_timer_get_time();

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "uptime_ms", (double)(now_us / 1000));
	cJSON *list = cJSON_AddArrayToObject(root, "events");
	for (size_t i = 0; i < n; i++) {
		cJSON *ev = cJSON_CreateObject();
		cJSON_AddNumberToObject(ev, "seq", events[i].seq);
		cJSON_AddNumberToObject(ev, "rise_ago_ms", (double)((now_us - events[i].rise_us) / 1000));
		if (events[i].ongoing) {
			cJSON_AddNullToObject(ev, "high_ms");
		} else {
			cJSON_AddNumberToObject(ev, "high_ms", events[i].high_ms);
		}
		cJSON_AddBoolToObject(ev, "pump_on", events[i].pump_on);
		cJSON_AddItemToArray(list, ev);
	}
	free(events);
	return send_json(req, 200, root);
}

static esp_err_t pump_on_handler(httpd_req_t *req) {
	char buf[MAX_BODY_LEN];
	cJSON *body = read_body_json(req, buf, sizeof(buf));
	if (body == NULL) {
		return ESP_OK;  // read_body_json already answered
	}

	uint32_t seconds = 0;
	bool found = false;
	const char *bad_field = NULL;
	if (!get_uint_field(body, "seconds", &seconds, &found, &bad_field)) {
		cJSON_Delete(body);
		return send_error(req, 400, "seconds must be a non-negative number");
	}
	cJSON_Delete(body);

	// 0 (or absent) tells the pump layer to use the configured manual duration.
	esp_err_t err = pump_request(PUMP_SOURCE_MANUAL, found ? seconds : 0);
	if (err == ESP_ERR_INVALID_ARG) {
		return send_error(req, 400, "seconds exceeds the safety cap");
	}
	if (err == ESP_ERR_NO_MEM) {
		return send_error(req, 409, "pump queue is full");
	}
	if (err != ESP_OK) {
		return send_error(req, 503, esp_err_to_name(err));
	}

	pump_config_t cfg;
	pump_get_config(&cfg);
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", true);
	cJSON_AddNumberToObject(root, "seconds",
	                        (found && seconds > 0) ? seconds : cfg.manual_run_seconds);
	return send_json(req, 200, root);
}

static esp_err_t pump_off_handler(httpd_req_t *req) {
	pump_stop();
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", true);
	return send_json(req, 200, root);
}

static esp_err_t config_get_handler(httpd_req_t *req) {
	pump_config_t cfg;
	pump_get_config(&cfg);
	cJSON *root = cJSON_CreateObject();
	add_config_fields(root, &cfg);
	return send_json(req, 200, root);
}

static esp_err_t config_post_handler(httpd_req_t *req) {
	char buf[MAX_BODY_LEN];
	cJSON *body = read_body_json(req, buf, sizeof(buf));
	if (body == NULL) {
		return ESP_OK;
	}

	// Start from the live config, so a partial body changes only what it names.
	pump_config_t cfg;
	pump_get_config(&cfg);

	bool found = false;
	const char *bad_field = NULL;
	if (!get_uint_field(body, "interval_seconds", &cfg.interval_seconds, &found, &bad_field) ||
	    !get_uint_field(body, "run_seconds", &cfg.run_seconds, &found, &bad_field) ||
	    !get_uint_field(body, "manual_run_seconds", &cfg.manual_run_seconds, &found, &bad_field) ||
	    !get_uint_field(body, "presence_confirm_pulses", &cfg.presence_confirm_pulses, &found,
	                    &bad_field) ||
	    !get_uint_field(body, "presence_confirm_window_seconds",
	                    &cfg.presence_confirm_window_seconds, &found, &bad_field) ||
	    !get_uint_field(body, "presence_hold_seconds", &cfg.presence_hold_seconds, &found,
	                    &bad_field) ||
	    !get_uint_field(body, "presence_max_session_seconds", &cfg.presence_max_session_seconds,
	                    &found, &bad_field) ||
	    !get_uint_field(body, "presence_rest_seconds", &cfg.presence_rest_seconds, &found,
	                    &bad_field) ||
	    !get_uint_field(body, "presence_hourly_budget_seconds",
	                    &cfg.presence_hourly_budget_seconds, &found, &bad_field)) {
		cJSON_Delete(body);
		char msg[80];
		snprintf(msg, sizeof(msg), "%s must be a non-negative number", bad_field);
		return send_error(req, 400, msg);
	}

	const cJSON *sensor_item = cJSON_GetObjectItemCaseSensitive(body, "presence_sensor");
	if (sensor_item != NULL && !cJSON_IsNull(sensor_item)) {
		if (!cJSON_IsString(sensor_item) ||
		    !presence_sensor_type_from_name(sensor_item->valuestring, &cfg.presence_sensor)) {
			cJSON_Delete(body);
			return send_error(req, 400, "presence_sensor must be pir or ld2410");
		}
	}

	// Sensor tuning, checked against the sensor this request leaves configured
	// - so switching to the radar and setting its range can go in one call.
	struct {
		const char *name;
		uint32_t *field;
	} tuning[] = {
		{"presence_max_gate", &cfg.presence_max_gate},
		{"presence_sensitivity", &cfg.presence_sensitivity},
		{"presence_no_one_seconds", &cfg.presence_no_one_seconds},
	};
	bool has_tuning = presence_sensor_for(cfg.presence_sensor)->has_tuning;
	for (size_t i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++) {
		char msg[80];
		if (!get_uint_field(body, tuning[i].name, tuning[i].field, &found, &bad_field)) {
			cJSON_Delete(body);
			snprintf(msg, sizeof(msg), "%s must be a non-negative number", bad_field);
			return send_error(req, 400, msg);
		}
		if (found && !has_tuning) {
			cJSON_Delete(body);
			snprintf(msg, sizeof(msg), "%s is not supported by %s", tuning[i].name,
			         presence_sensor_type_name(cfg.presence_sensor));
			return send_error(req, 400, msg);
		}
	}

	if (!get_bool_field(body, "schedule_enabled", &cfg.schedule_enabled, &found, &bad_field)) {
		cJSON_Delete(body);
		return send_error(req, 400, "schedule_enabled must be true or false");
	}

	const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(body, "mode");
	if (mode_item != NULL && !cJSON_IsNull(mode_item)) {
		if (!cJSON_IsString(mode_item) || !pump_mode_from_name(mode_item->valuestring, &cfg.mode)) {
			cJSON_Delete(body);
			return send_error(req, 400, "mode must be interval, on_demand or presence");
		}
	}
	cJSON_Delete(body);

	char reason[64];
	if (!pump_config_validate(&cfg, reason, sizeof(reason))) {
		return send_error(req, 400, reason);
	}

	esp_err_t err = pump_config_save(&cfg);
	if (err != ESP_OK) {
		return send_error(req, 503, "could not persist config");
	}
	pump_apply_config(&cfg);
	// Best-effort: the sensor being unreachable shouldn't fail a config save
	// that otherwise took - presence mode runs off cfg either way.
	presence_apply_config(&cfg);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", true);
	cJSON_AddBoolToObject(root, "restart_required", restart_required(&cfg));
	add_config_fields(root, &cfg);
	return send_json(req, 200, root);
}

esp_err_t http_api_start(void) {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.lru_purge_enable = true;
	// Handlers keep a MAX_BODY_LEN buffer and the config on the stack; the
	// 4 KB default leaves too little headroom for cJSON on top.
	config.stack_size = 6144;
	// Four GETs and three POSTs, registered below.
	config.max_uri_handlers = 8;

	httpd_handle_t server = NULL;
	esp_err_t err = httpd_start(&server, &config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
		return err;
	}

	static const httpd_uri_t routes[] = {
		{.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
		{.uri = "/api/history", .method = HTTP_GET, .handler = history_get_handler},
		{.uri = "/api/presence/events", .method = HTTP_GET, .handler = presence_events_get_handler},
		{.uri = "/api/pump/on", .method = HTTP_POST, .handler = pump_on_handler},
		{.uri = "/api/pump/off", .method = HTTP_POST, .handler = pump_off_handler},
		{.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler},
		{.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler},
	};
	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
		ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
	}

	ESP_LOGI(TAG, "REST API listening on port 80");
	return ESP_OK;
}
