#pragma once

#include "esp_err.h"

// Starts the REST server on port 80. Endpoints are documented in README.md.
esp_err_t http_api_start(void);
