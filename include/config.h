#pragma once

// ---- Hardware ----

// GPIO driving the relay module's IN pin.
static const int RELAY_PIN = 4;

// Opto-isolated module: a LOW on IN closes COM-NO (pump runs).
// If the relay clicks opposite to what the logs say, flip this to false.
static const bool RELAY_ACTIVE_LOW = true;

// ---- Watering cycle ----

// How long the pump runs in one cycle.
static const unsigned long PUMP_ON_SECONDS = 10;

// Gap between cycles. 360 min = 6 h.
static const unsigned long PUMP_INTERVAL_MINUTES = 360;

// Water immediately on boot? false means a reset waits out a full interval,
// so a reset loop can't flood the plants.
static const bool RUN_ON_BOOT = false;

// ---- Safety ----

// Hard cap on run time. If PUMP_ON_SECONDS is ever set to something absurd,
// the pump still shuts off here.
static const unsigned long PUMP_MAX_ON_SECONDS = 60;
