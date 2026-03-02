# 🔧 Instrukcje modyfikacji kodu ESP32

## Wymagane zmiany w Twoim projekcie Arduino

### 1. Dodaj nowe pliki do projektu
Skopiuj te pliki do folderu `004_Wedzarnia_2xds18b20_NTC_web_pass_FLASH/`:
- `cloud_report.h` + `cloud_report.cpp` — wysyłanie danych do chmury
- `api_endpoint.h` + `api_endpoint.cpp` — JSON API dla aplikacji mobilnej

### 2. Zainstaluj bibliotekę ArduinoJson
W Arduino IDE: Szkic → Dołącz bibliotekę → Zarządzaj → wyszukaj `ArduinoJson` i zainstaluj (v7+)

### 3. Dodaj do głównego pliku .ino

```cpp
#include "cloud_report.h"
#include "api_endpoint.h"

void setup() {
  // ... istniejący kod setup ...
  
  // Dodaj na końcu setup():
  cloudReportSetup();
  setupApiEndpoints(server);  // 'server' to Twój WebServer
}

void loop() {
  // ... istniejący kod loop ...
  
  // Dodaj w loop():
  cloudReportLoop();  // Wysyła dane do chmury co 5 sekund
}
```

### 4. Dostosuj `state.h` — dodaj metodę processStateStr()

```cpp
// Dodaj do klasy/struktury state:
const char* processStateStr() {
  switch (processState) {
    case IDLE: return "IDLE";
    case RUNNING_AUTO: return "RUNNING_AUTO";
    case RUNNING_MANUAL: return "RUNNING_MANUAL";
    case PAUSED: return "PAUSED";
    case DONE: return "DONE";
    case ERROR_STATE: return "ERROR";
    default: return "IDLE";
  }
}
```

### 5. Skonfiguruj klucz urządzenia

1. W `cloud_report.h` ustaw `CLOUD_DEVICE_KEY` na dowolny klucz (np. `"mojaWedzarnia2024"`)
2. W Lovable Cloud dodaj secret `SMOKER_DEVICE_KEY` z tym samym kluczem

### 6. Jak to działa

```
┌─────────────┐     WiFi LAN      ┌──────────────┐
│   ESP32      │◄─────────────────►│  Aplikacja   │
│  Wędzarnia   │   /api/status     │   PWA na     │
│              │                   │   telefonie  │
│              │     Internet      │              │
│              │──────────────────►│  Lovable     │
│              │  cloud_report     │  Cloud (DB)  │
└─────────────┘                   └──────────────┘

Tryb AUTO:
1. Aplikacja próbuje połączyć się lokalnie (WiFi LAN)
2. Jeśli nie ma dostępu → pobiera dane z chmury
3. ESP32 zawsze wysyła dane do obu: LAN + chmura
```

### 7. Testowanie

1. Wgraj firmware na ESP32
2. Otwórz aplikację na telefonie
3. W ustawieniach połączenia (ikona ⚙️) wpisz IP ESP32
4. Odznacz "Tryb symulacji"
5. Wybierz tryb: Auto / WiFi / Chmura
