#include "ld2410_uart.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "ld2410";

// Protocol from the HLK-LD2410 datasheet's serial command set (same framing
// used by the ncmreynolds/ld2410 and ESPHome ld2410 implementations).
static const uint8_t CMD_HEADER[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t CMD_FOOTER[4] = {0x04, 0x03, 0x02, 0x01};

#define CMD_ENABLE_CONFIG 0xFF
#define CMD_END_CONFIG 0xFE
#define CMD_MAX_DISTANCE_DURATION 0x60
#define CMD_GATE_SENSITIVITY 0x64

#define UART_RX_BUF_SIZE 256
#define UART_TX_BUF_SIZE 256
#define ACK_TIMEOUT_MS 500

static bool s_installed;

static esp_err_t send_frame(const uint8_t *payload, size_t payload_len) {
	uart_write_bytes(LD2410_UART_PORT, (const char *)CMD_HEADER, sizeof(CMD_HEADER));
	uint8_t len_le[2] = {(uint8_t)(payload_len & 0xFF), (uint8_t)((payload_len >> 8) & 0xFF)};
	uart_write_bytes(LD2410_UART_PORT, (const char *)len_le, sizeof(len_le));
	uart_write_bytes(LD2410_UART_PORT, (const char *)payload, payload_len);
	uart_write_bytes(LD2410_UART_PORT, (const char *)CMD_FOOTER, sizeof(CMD_FOOTER));
	return uart_wait_tx_done(LD2410_UART_PORT, pdMS_TO_TICKS(100));
}

// Reads bytes until an ack frame for `opcode` shows up or `timeout_ms`
// elapses. Ack layout: FD FC FB FA | len_lo len_hi | opcode 0x01 | status_lo
// status_hi | ... | 04 03 02 01. status 0x0000 means success.
static bool wait_for_ack(uint8_t opcode, uint32_t timeout_ms) {
	uint8_t frame[32];
	size_t have = 0;
	TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

	while (xTaskGetTickCount() < deadline) {
		uint8_t byte;
		int n = uart_read_bytes(LD2410_UART_PORT, &byte, 1, pdMS_TO_TICKS(20));
		if (n <= 0) {
			continue;
		}

		if (have == 0 && byte != CMD_HEADER[0]) {
			continue;  // not a command frame - skip (likely a data frame byte)
		}
		if (have < sizeof(frame)) {
			frame[have++] = byte;
		}

		// Header (4) + length (2) + opcode/ack (2) + status (2) = 10 bytes is
		// enough to judge success; we don't need the footer for that.
		if (have >= 10 && frame[0] == CMD_HEADER[0] && frame[1] == CMD_HEADER[1] &&
		    frame[2] == CMD_HEADER[2] && frame[3] == CMD_HEADER[3]) {
			bool right_ack = (frame[6] == opcode && frame[7] == 0x01);
			bool ok = (frame[8] == 0x00 && frame[9] == 0x00);
			if (right_ack) {
				return ok;
			}
			// Some other frame's tail happened to start with our header byte;
			// reset and keep scanning within the timeout.
			have = 0;
		}
	}
	ESP_LOGW(TAG, "ack 0x%02X timed out", opcode);
	return false;
}

static bool enter_config_mode(void) {
	const uint8_t payload[] = {0xFF, 0x00, 0x01, 0x00};
	if (send_frame(payload, sizeof(payload)) != ESP_OK) {
		return false;
	}
	return wait_for_ack(CMD_ENABLE_CONFIG, ACK_TIMEOUT_MS);
}

static bool leave_config_mode(void) {
	const uint8_t payload[] = {0xFE, 0x00};
	if (send_frame(payload, sizeof(payload)) != ESP_OK) {
		return false;
	}
	return wait_for_ack(CMD_END_CONFIG, ACK_TIMEOUT_MS);
}

// Sets both the moving and stationary max-distance gate to the same value,
// plus the no-one (inactivity) timeout. Payload is three (param word, u32
// value) pairs: param 0 = moving gate, 1 = stationary gate, 2 = no-one secs.
static bool set_max_distance_and_timeout(uint16_t gate, uint16_t no_one_seconds) {
	uint8_t payload[26] = {
		0x60, 0x00,
		0x00, 0x00, (uint8_t)(gate & 0xFF), (uint8_t)((gate >> 8) & 0xFF), 0x00, 0x00,
		0x01, 0x00, (uint8_t)(gate & 0xFF), (uint8_t)((gate >> 8) & 0xFF), 0x00, 0x00,
		0x02, 0x00, (uint8_t)(no_one_seconds & 0xFF), (uint8_t)((no_one_seconds >> 8) & 0xFF), 0x00, 0x00,
	};
	if (send_frame(payload, 20) != ESP_OK) {
		return false;
	}
	return wait_for_ack(CMD_MAX_DISTANCE_DURATION, ACK_TIMEOUT_MS);
}

// One gate's moving/stationary sensitivity per call - the protocol has no
// "all gates" wildcard for this command, so the caller loops 0..max_gate.
static bool set_gate_sensitivity(uint8_t gate, uint8_t sensitivity) {
	uint8_t payload[20] = {
		0x64, 0x00,
		0x00, 0x00, gate, 0x00, 0x00, 0x00,
		0x01, 0x00, sensitivity, 0x00, 0x00, 0x00,
		0x02, 0x00, sensitivity, 0x00, 0x00, 0x00,
	};
	if (send_frame(payload, 20) != ESP_OK) {
		return false;
	}
	return wait_for_ack(CMD_GATE_SENSITIVITY, ACK_TIMEOUT_MS);
}

esp_err_t ld2410_uart_init(void) {
	if (s_installed) {
		return ESP_OK;
	}

	uart_config_t uart_cfg = {
		.baud_rate = LD2410_UART_BAUD_RATE,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	esp_err_t err = uart_driver_install(LD2410_UART_PORT, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
	if (err != ESP_OK) {
		return err;
	}
	err = uart_param_config(LD2410_UART_PORT, &uart_cfg);
	if (err != ESP_OK) {
		return err;
	}
	err = uart_set_pin(LD2410_UART_PORT, LD2410_UART_TXD_GPIO, LD2410_UART_RXD_GPIO,
	                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	if (err != ESP_OK) {
		return err;
	}

	s_installed = true;
	ESP_LOGI(TAG, "UART up on port %d (TX=%d RX=%d, %d baud)", LD2410_UART_PORT,
	         LD2410_UART_TXD_GPIO, LD2410_UART_RXD_GPIO, LD2410_UART_BAUD_RATE);
	return ESP_OK;
}

esp_err_t ld2410_uart_apply_config(const pump_config_t *cfg) {
	if (!s_installed) {
		return ESP_ERR_INVALID_STATE;
	}

	if (!enter_config_mode()) {
		ESP_LOGW(TAG, "sensor did not ack config mode - leaving its settings as-is");
		return ESP_ERR_TIMEOUT;
	}

	bool ok = set_max_distance_and_timeout((uint16_t)cfg->presence_max_gate,
	                                       (uint16_t)cfg->presence_no_one_seconds);
	for (uint32_t gate = 0; ok && gate <= cfg->presence_max_gate; gate++) {
		ok = set_gate_sensitivity((uint8_t)gate, (uint8_t)cfg->presence_sensitivity);
	}

	leave_config_mode();  // always try to leave config mode, even on failure

	if (!ok) {
		ESP_LOGW(TAG, "sensor rejected or did not ack tuning - some settings may be stale");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "pushed gate=%lu sensitivity=%lu no_one=%lus to sensor",
	         (unsigned long)cfg->presence_max_gate, (unsigned long)cfg->presence_sensitivity,
	         (unsigned long)cfg->presence_no_one_seconds);
	return ESP_OK;
}
