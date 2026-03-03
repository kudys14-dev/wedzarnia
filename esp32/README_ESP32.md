# 🔧 Instrukcje integracji ESP32 z aplikacją Wędzarnia IoT

## Wymagane zmiany w projekcie Arduino

### 1. Skopiuj nowe pliki do projektu
Skopiuj do folderu `004a_Wedzarnia_2xds18b20_NTC_web_pass_FLASH_tesst/`:
- `cloud_report.h` + `cloud_report.cpp` — wysyłanie danych do chmury (Supabase)
- `api_endpoint.h` + `api_endpoint.cpp` — JSON API dla aplikacji mobilnej (WiFi LAN)

### 2. Zainstaluj wymagane biblioteki
W Arduino IDE: *Szkic → Dołącz bibliotekę → Zarządzaj bibliotekamii*
- **ArduinoJson** v7+ (autor: Benoit Blanchon)

### 3. Dodaj do głównego pliku `.ino`

```cpp
#include "cloud_report.h"
#include "api_endpoint.h"

void setup() {
  // ... istniejący kod setup() ...

  // Dodaj na końcu setup(), po inicjalizacji serwera WWW:
  cloudReportSetup();
  setupApiEndpoints(server);  // 'server' to Twój obiekt WebServer
}

void loop() {
  // ... istniejący kod loop() ...

  // Dodaj w loop():
  cloudReportLoop();  // Wysyła dane do chmury co 5 sekund
  server.handleClient();
}
```

### 4. Poprawna wersja `processStateStr()` w `state.h`

> ⚠️ **Ważne:** Użyj poniższego kodu. Stara wersja z wartościami `PAUSED`, `DONE`, `ERROR_STATE`
> nie kompiluje się — te wartości nie istnieją w enumie `ProcessState`.

```cpp
// W state.h — dodaj słowo kluczowe 'inline' aby uniknąć błędu "multiple definition"
inline const char* processStateStr() {
  switch (g_currentState) {
    case ProcessState::IDLE:               return "IDLE";
    case ProcessState::RUNNING_AUTO:       return "RUNNING_AUTO";
    case ProcessState::RUNNING_MANUAL:     return "RUNNING_MANUAL";
    case ProcessState::PAUSE_DOOR:         return "PAUSE_DOOR";
    case ProcessState::PAUSE_SENSOR:       return "PAUSE_SENSOR";
    case ProcessState::PAUSE_OVERHEAT:     return "PAUSE_OVERHEAT";
    case ProcessState::PAUSE_USER:         return "PAUSE_USER";
    case ProcessState::ERROR_PROFILE:      return "ERROR_PROFILE";
    case ProcessState::SOFT_RESUME:        return "SOFT_RESUME";
    case ProcessState::PAUSE_HEATER_FAULT: return "PAUSE_HEATER_FAULT";
    default:                               return "IDLE";
  }
}
```

### 5. Skonfiguruj klucz urządzenia

1. W pliku `cloud_report.h` zmień wartość `CLOUD_DEVICE_KEY`:
   ```cpp
   #define CLOUD_DEVICE_KEY "twoj_unikalny_klucz"
   ```
2. W panelu Supabase → *Edge Functions → Secrets* dodaj:
   - Klucz: `SMOKER_DEVICE_KEY`
   - Wartość: ten sam ciąg co powyżej

### 6. Obsługiwane komendy API (`POST /api/command`)

Aplikacja wysyła JSON `{"cmd": "...", "value": ...}` na adres `http://<IP>/api/command`:

| `cmd`       | Opis                          | `value`          |
|-------------|-------------------------------|------------------|
| `start`     | Uruchomienie procesu          | —                |
| `pause`     | Wstrzymanie procesu           | —                |
| `resume`    | Wznowienie po pauzie          | —                |
| `stop`      | Zatrzymanie procesu           | —                |
| `next_step` | Przejście do następnego kroku | —                |
| `set_temp`  | Zmiana temperatury zadanej    | float 30–120 °C  |

### 7. Jak działa połączenie

```
┌─────────────┐    WiFi LAN (bezpośrednio)    ┌──────────────┐
│    ESP32     │ ◄───────────────────────────► │  Aplikacja   │
│  Wędzarnia  │   GET  /api/status            │  PWA         │
│             │   POST /api/command           │  (telefon)   │
│             │                               │              │
│             │    Internet (przez router)    │              │
│             │ ──────────────────────────►   │  Supabase    │
│             │   POST cloud_report           │  (chmura)    │
└─────────────┘                               └──────────────┘

Tryb AUTO w aplikacji:
1. Próba połączenia lokalnego (WiFi LAN) — najniższe opóźnienie
2. Jeśli brak → dane z chmury (Supabase) — działa przez internet
3. Jeśli brak obu → wyświetlana symulacja + ostrzeżenie w UI
```

### 8. Testowanie po wgraniu firmware

1. Otwórz aplikację na telefonie
2. Kliknij ikonę ⚙️ w prawym górnym rogu
3. Odznacz *Tryb symulacji (demo)*
4. Wpisz adres IP ESP32 (widoczny w Serial Monitor przy starcie)
5. Ustaw tryb: **Auto** (zalecany) lub **WiFi LAN**
6. Zielony wskaźnik = połączono lokalnie

### 9. Wymagania sieciowe

- Telefon i ESP32 muszą być w tej samej sieci WiFi dla trybu **lokalnego**
- Dla trybu **chmura** wystarczy dostęp ESP32 do internetu
- Porty: ESP32 nasłuchuje na porcie **80** (HTTP)
