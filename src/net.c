#include "net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include "wifi_secrets.h"

static const char *TAG = "net";

static volatile bool s_connected;
static uint32_t s_retry_delay_ms = 1000;

// Backoff ceiling. The fountain may sit for hours with the router down; retrying
// every second for that long is pointless noise, but we never give up entirely.
#define RETRY_DELAY_MAX_MS 60000

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
	(void)arg;
	(void)data;

	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
		return;
	}

	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		s_connected = false;
		ESP_LOGW(TAG, "disconnected, retrying in %lu ms", (unsigned long)s_retry_delay_ms);
		// Blocking the event loop is acceptable here: the pump task runs at its
		// own priority and does not depend on this one.
		vTaskDelay(pdMS_TO_TICKS(s_retry_delay_ms));
		if (s_retry_delay_ms < RETRY_DELAY_MAX_MS) {
			s_retry_delay_ms *= 2;
			if (s_retry_delay_ms > RETRY_DELAY_MAX_MS) {
				s_retry_delay_ms = RETRY_DELAY_MAX_MS;
			}
		}
		esp_wifi_connect();
		return;
	}

	if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
		s_connected = true;
		s_retry_delay_ms = 1000;
		ESP_LOGI(TAG, "connected, IP " IPSTR " - http://" DEVICE_HOSTNAME ".local",
		         IP2STR(&event->ip_info.ip));
	}
}

static esp_err_t mdns_start(void) {
	esp_err_t err = mdns_init();
	if (err != ESP_OK) {
		return err;
	}
	ESP_ERROR_CHECK(mdns_hostname_set(DEVICE_HOSTNAME));
	ESP_ERROR_CHECK(mdns_instance_name_set("Water pump controller"));
	ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
	ESP_LOGI(TAG, "mDNS up as %s.local", DEVICE_HOSTNAME);
	return ESP_OK;
}

esp_err_t net_init(void) {
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
	                                                    &wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
	                                                    &wifi_event_handler, NULL, NULL));

	wifi_config_t wifi_cfg = {0};
	strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
	strlcpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
	// The radio staying awake keeps the REST API responsive; the board is mains
	// powered, so there is nothing to save by letting it doze between beacons.
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_start());

	esp_err_t err = mdns_start();
	if (err != ESP_OK) {
		// Losing mDNS only costs the friendly name; the IP still works.
		ESP_LOGW(TAG, "mDNS failed to start: %s", esp_err_to_name(err));
	}

	ESP_LOGI(TAG, "WiFi started, associating with \"%s\"", WIFI_SSID);
	return ESP_OK;
}

bool net_is_connected(void) { return s_connected; }

int net_rssi(void) {
	wifi_ap_record_t ap;
	if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
		return 0;
	}
	return ap.rssi;
}
