#pragma once

// ---- Sprzet ----

// GPIO sterujacy wejsciem IN modulu przekaznika.
static const int RELAY_PIN = 4;

// Modul z optoizolacja: stan NISKI na IN zwiera COM-NO (pompa pracuje).
// Jesli przekaznik klika odwrotnie niz logi - zmien na false.
static const bool RELAY_ACTIVE_LOW = true;

// ---- Cykl podlewania ----

// Jak dlugo pompa pracuje w jednym cyklu.
static const unsigned long PUMP_ON_SECONDS = 10;

// Przerwa miedzy cyklami. 360 min = 6 h.
static const unsigned long PUMP_INTERVAL_MINUTES = 360;

// Czy podlac od razu po starcie. false = po resecie czekamy pelny interwal,
// zeby seria resetow nie zalala roslin.
static const bool RUN_ON_BOOT = false;

// ---- Bezpiecznik ----

// Twardy limit czasu pracy. Jesli PUMP_ON_SECONDS zostanie ustawione przez
// pomylke na absurdalna wartosc, pompa i tak wylaczy sie tutaj.
static const unsigned long PUMP_MAX_ON_SECONDS = 60;
