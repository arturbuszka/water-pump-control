#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "pump_config.h"

// Talks to the HLK-LD2410C's own UART (TX/RX, wired per LD2410_UART_* in
// config.h) to push sensitivity/range tuning into the sensor. This is
// separate from PRESENCE_GPIO, which is the sensor's simple digital OUT pin
// and is all the presence task needs at runtime - this module only matters
// when the tuning changes.

// Installs the second UART driver. Call once at boot, before the first
// ld2410_uart_apply_config().
esp_err_t ld2410_uart_init(void);

// Enters the sensor's configuration mode, pushes presence_max_gate /
// presence_sensitivity / presence_no_one_seconds from `cfg`, and leaves
// configuration mode again. Logs a warning and returns an error without
// crashing if the sensor does not ack in time - a misbehaving radar should
// not take the pump down with it.
esp_err_t ld2410_uart_apply_config(const pump_config_t *cfg);
