#include <Arduino.h>

#include "config.h"

enum PumpState { IDLE, PUMPING };

static PumpState state = IDLE;

// When the current phase (running or waiting) began.
static unsigned long phaseStartedAt = 0;

// Effective run time - PUMP_ON_SECONDS clamped to the safety cap.
static unsigned long onMs = 0;
static unsigned long intervalMs = 0;

// The only place that knows the module's polarity. The rest of the code thinks
// in terms of "pump running / not running".
static void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
}

static void startPumping(unsigned long now) {
  state = PUMPING;
  phaseStartedAt = now;
  relayWrite(true);
  Serial.printf("[%lu ms] PUMP ON for %lu s\n", now, onMs / 1000UL);
}

static void stopPumping(unsigned long now) {
  state = IDLE;
  phaseStartedAt = now;
  relayWrite(false);
  Serial.printf("[%lu ms] PUMP OFF, next cycle in %lu s\n", now,
                intervalMs / 1000UL);
}

void setup() {
  // Set the level BEFORE pinMode. The other order leaves the pin floating for a
  // moment, which is enough to make the relay click on every boot.
  relayWrite(false);
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);

  Serial.begin(115200);

  onMs = min(PUMP_ON_SECONDS, PUMP_MAX_ON_SECONDS) * 1000UL;
  intervalMs = PUMP_INTERVAL_MINUTES * 60UL * 1000UL;

  // Give the USB host a moment, but don't block forever - the controller has to
  // run headless too.
  unsigned long waitStart = millis();
  while (!Serial && millis() - waitStart < 2000UL) {
    delay(10);
  }

  Serial.println();
  Serial.println("=== Water pump controller (XIAO ESP32-C3) ===");
  Serial.printf("GPIO %d, triggered by %s level\n", RELAY_PIN,
                RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.printf("Run: %lu s, interval: %lu min\n", onMs / 1000UL,
                PUMP_INTERVAL_MINUTES);

  if (PUMP_ON_SECONDS > PUMP_MAX_ON_SECONDS) {
    Serial.printf("WARNING: PUMP_ON_SECONDS (%lu) clamped to safety cap %lu s\n",
                  PUMP_ON_SECONDS, PUMP_MAX_ON_SECONDS);
  }

  unsigned long now = millis();
  if (RUN_ON_BOOT) {
    startPumping(now);
  } else {
    state = IDLE;
    phaseStartedAt = now;
    Serial.printf("Starting idle, first cycle in %lu s\n", intervalMs / 1000UL);
  }
}

void loop() {
  unsigned long now = millis();

  // Unsigned subtraction survives the millis() rollover (~49 days).
  unsigned long elapsed = now - phaseStartedAt;

  switch (state) {
    case PUMPING:
      if (elapsed >= onMs) {
        stopPumping(now);
      }
      break;

    case IDLE:
      if (elapsed >= intervalMs) {
        startPumping(now);
      }
      break;
  }

  delay(50);
}
