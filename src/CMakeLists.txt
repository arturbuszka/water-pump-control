idf_component_register(
	SRCS "main.c" "pump.c" "pump_config.c" "net.c" "http_api.c" "presence.c" "presence_logic.c" "quiet_hours.c" "wall_clock.c" "sensor_pir.c" "sensor_ld2410.c" "ld2410_uart.c"
	INCLUDE_DIRS "." "../include"
	# The phone UI, served at / - one self-contained page, see web/index.html.
	EMBED_FILES "../web/index.html"
	REQUIRES driver esp_netif esp_wifi esp_http_server esp_timer nvs_flash mdns cjson
)
