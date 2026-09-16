# waterpumpmod

Sterownik pompki wody 5V na **Seeed Studio XIAO ESP32-C3** + moduł przekaźnika
1-kanałowego z optoizolacją (styki 10A/250VAC, cewka 5V).

Tryb pracy: **interwał czasowy**. Bez WiFi, bez UI — pompa włącza się co zadany
czas i pracuje przez zadaną liczbę sekund.

## Połączenia

```
USB 5V ──┬──────────────────────── XIAO  5V pad
         │
         ├──────────────────────── PRZEKAŹNIK  VCC
         │
         └── PRZECIĘTY przewód + pompy ──► PRZEKAŹNIK  COM
                                           PRZEKAŹNIK  NO ──► pompa  +

GND ─────┬──────────────────────── XIAO  GND
         ├──────────────────────── PRZEKAŹNIK  GND
         └──────────────────────── pompa  −      (NIE przecinamy)

XIAO GPIO4 ─────────────────────── PRZEKAŹNIK  IN
```

### Zasady, których nie warto łamać

- **Przecinamy tylko przewód dodatni (+) pompy.** Masa musi zostać ciągła i wspólna
  dla XIAO, przekaźnika i pompy. Bez wspólnej masy sygnał na IN nie ma odniesienia
  i przekaźnik zachowuje się losowo.
- **Używamy NO, nie NC.** Przy braku zasilania albo resecie MCU styki są rozwarte,
  więc pompa stoi. Na NC byłoby odwrotnie — reset oznaczałby zalanie.
- Moduł jest **active-LOW**: stan niski na IN zwiera COM↔NO.

### Dwie rzeczy, które mogą nie zadziałać od razu

**GPIO4 daje 3.3V, moduł jest 5V.** Większość modułów z optoizolacją załącza się
od 3.3V, ale nie wszystkie. Jeśli test na sucho nie daje kliknięcia, wstaw driver:

```
GPIO4 ──[ 1 kΩ ]── baza  2N2222
                   emiter ── GND
                   kolektor ── IN przekaźnika
```
Uwaga: tranzystor **odwraca** sygnał, więc wtedy ustaw `RELAY_ACTIVE_LOW = false`.

**Brownout.** Pompa i XIAO ciągną z tego samego USB. Jeśli start pompy resetuje
płytkę, dołóż kondensator 470–1000 µF na szynie 5V albo zasil pompę osobno,
zachowując wspólną masę.

## Konfiguracja

Wszystkie nastawy w [include/config.h](include/config.h):

| Stała | Domyślnie | Znaczenie |
|---|---|---|
| `RELAY_PIN` | `4` | GPIO sterujący wejściem IN |
| `RELAY_ACTIVE_LOW` | `true` | Stan niski załącza przekaźnik |
| `PUMP_ON_SECONDS` | `10` | Czas pracy pompy w cyklu |
| `PUMP_INTERVAL_MINUTES` | `360` | Przerwa między cyklami (6 h) |
| `RUN_ON_BOOT` | `false` | Czy podlać od razu po starcie |
| `PUMP_MAX_ON_SECONDS` | `60` | Twardy limit — przycina `PUMP_ON_SECONDS` |

## Budowanie

```sh
pio run                  # kompilacja
pio run -t upload        # wgranie
pio device monitor       # logi, 115200
```

Jeśli płytka nie wchodzi w tryb bootloadera: przytrzymaj **BOOT**, wepnij USB-C,
puść BOOT.

## Test na sucho — zrób to przed podłączeniem pompy

1. Podłącz **tylko** XIAO i przekaźnik (VCC / GND / IN). Do COM i NO nic nie wpinaj.
2. W `config.h` ustaw tymczasowo:
   `PUMP_ON_SECONDS = 3`, `PUMP_INTERVAL_MINUTES = 0`, `RUN_ON_BOOT = true`.
3. `pio run -t upload && pio device monitor`
4. Oczekiwane: słyszalny **klik**, dioda modułu świeci 3 s, klik z powrotem —
   i logi na Serial zgodne z klikaniem.
   - Brak kliknięć → 3.3V nie wystarcza, zastosuj driver tranzystorowy powyżej.
   - Klika odwrotnie niż logi → zmień `RELAY_ACTIVE_LOW`.
5. Multimetr w trybie ciągłości na **COM–NO**: rozwarte gdy OFF, zwarte gdy ON.
   To potwierdza, że korzystasz z właściwej pary styków.
6. Dopiero teraz wepnij pompę i obejrzyj jeden pełny cykl.
7. Przywróć docelowe `PUMP_ON_SECONDS` / `PUMP_INTERVAL_MINUTES`, ustaw
   `RUN_ON_BOOT = false`, wgraj ponownie.

## Uwaga

Odliczanie opiera się na `millis()` — dryfuje i zeruje się po zaniku zasilania.
Do podlewania roślin to bez znaczenia. Gdyby kiedyś była potrzebna konkretna pora
dnia, trzeba dołożyć WiFi + NTP albo układ RTC.
