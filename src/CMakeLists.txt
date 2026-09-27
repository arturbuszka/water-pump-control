idf_component_register(
	SRCS "main.c" "pump.c" "pump_config.c" "net.c" "http_api.c" "presence.c" "ld2410_uart.c"
	INCLUDE_DIRS "." "../include"
	REQUIRES driver esp_wifi esp_http_server esp_timer nvs_flash mdns cjson
)
