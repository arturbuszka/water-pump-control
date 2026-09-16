#include <Arduino.h>

#include "config.h"

enum PumpState { IDLE, PUMPING };

static PumpState state = IDLE;

// Znacznik poczatku biezacej fazy (pracy albo przerwy).
static unsigned long phaseStartedAt = 0;

// Efektywny czas pracy - PUMP_ON_SECONDS przyciete do bezpiecznika.
static unsigned long onMs = 0;
static unsigned long intervalMs = 0;

// Jedyne miejsce znajace polaryzacje modulu. Reszta kodu mysli w kategoriach
// "pompa pracuje / nie pracuje".
static void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
}

static void startPumping(unsigned long now) {
  state = PUMPING;
  phaseStartedAt = now;
  relayWrite(true);
  Serial.printf("[%lu ms] POMPA ON na %lu s\n", now, onMs / 1000UL);
}

static void stopPumping(unsigned long now) {
  state = IDLE;
  phaseStartedAt = now;
  relayWrite(false);
  Serial.printf("[%lu ms] POMPA OFF, nastepny cykl za %lu s\n", now,
                intervalMs / 1000UL);
}

void setup() {
  // Ustawiamy poziom PRZED pinMode. Odwrotna kolejnosc daje krotki impuls na
  // wyjsciu i przekaznik potrafi kliknac przy starcie.
  relayWrite(false);
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);

  Serial.begin(115200);

  onMs = min(PUMP_ON_SECONDS, PUMP_MAX_ON_SECONDS) * 1000UL;
  intervalMs = PUMP_INTERVAL_MINUTES * 60UL * 1000UL;

  // Czekamy chwile na host USB, ale nie blokujemy sie w nieskonczonosc -
  // sterownik ma dzialac takze bez podlaczonego komputera.
  unsigned long waitStart = millis();
  while (!Serial && millis() - waitStart < 2000UL) {
    delay(10);
  }

  Serial.println();
  Serial.println("=== Sterownik pompy (XIAO ESP32-C3) ===");
  Serial.printf("GPIO %d, wyzwalanie stanem %s\n", RELAY_PIN,
                RELAY_ACTIVE_LOW ? "NISKIM" : "WYSOKIM");
  Serial.printf("Praca: %lu s, interwal: %lu min\n", onMs / 1000UL,
                PUMP_INTERVAL_MINUTES);

  if (PUMP_ON_SECONDS > PUMP_MAX_ON_SECONDS) {
    Serial.printf("UWAGA: PUMP_ON_SECONDS (%lu) przyciete do bezpiecznika %lu s\n",
                  PUMP_ON_SECONDS, PUMP_MAX_ON_SECONDS);
  }

  unsigned long now = millis();
  if (RUN_ON_BOOT) {
    startPumping(now);
  } else {
    state = IDLE;
    phaseStartedAt = now;
    Serial.printf("Start w trybie czuwania, pierwszy cykl za %lu s\n",
                  intervalMs / 1000UL);
  }
}

void loop() {
  unsigned long now = millis();

  // Odejmowanie na unsigned long jest odporne na przepelnienie millis (~49 dni).
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
