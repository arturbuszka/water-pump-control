#include "wall_clock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "config.h"

static const char *TAG = "clock";

// 2024-01-01 UTC. An unset RTC counts up from 1970, so anything earlier than
// this firmware can possibly be running means nobody has set the clock.
#define WALL_CLOCK_VALID_AFTER 1704067200

static void on_sync(struct timeval *tv) {
	ESP_LOGI(TAG, "time synced: %lld", (long long)tv->tv_sec);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
	(void)arg;
	(void)base;
	(void)id;
	(void)data;
	// SNTP's retries back off while there is no network; ask again right away
	// once there is one, rather than waiting the backoff out.
	esp_err_t err = esp_netif_sntp_start();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "SNTP restart failed: %s", esp_err_to_name(err));
	}
}

void wall_clock_init(void) {
	setenv("TZ", WALL_CLOCK_TZ, 1);
	tzset();
}

esp_err_t wall_clock_start_sync(void) {
	esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(WALL_CLOCK_NTP_SERVER);
	// Nothing here blocks on a sync, so no semaphore is needed for one.
	config.wait_for_sync = false;
	config.sync_cb = on_sync;
	esp_err_t err = esp_netif_sntp_init(&config);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "SNTP failed to start: %s", esp_err_to_name(err));
		return err;
	}
	return esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL, NULL);
}

bool wall_clock_second_of_day(uint32_t *out) {
	time_t now = time(NULL);
	if (now < WALL_CLOCK_VALID_AFTER) {
		return false;
	}
	struct tm local;
	localtime_r(&now, &local);
	*out = (uint32_t)local.tm_hour * 3600 + (uint32_t)local.tm_min * 60 + (uint32_t)local.tm_sec;
	return true;
}
