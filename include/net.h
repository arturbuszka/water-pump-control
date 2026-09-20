#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Brings up WiFi in station mode and starts mDNS, so the board answers to
// http://waterpump.local. Returns as soon as the connection attempt is under
// way - it does not block until an IP arrives, because watering must start on
// schedule whether or not the network is up.
esp_err_t net_init(void);

bool net_is_connected(void);

// Signal strength of the current association, or 0 when disconnected.
int net_rssi(void);
