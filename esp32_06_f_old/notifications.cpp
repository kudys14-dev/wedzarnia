// notifications.cpp - Powiadomienia push przez ntfy.sh
// v1.0 - alerty temperatury, mięsa, awarii, startu/stopu procesu
#include "notifications.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>

// =================================================================
// KONFIGURACJA - zmień w config.h lub tutaj
// Domyślny topic: "wedzarnia-<ostatnie 4 bajty MAC>"
// Po uruchomieniu zobaczysz topic w Serial Monitor
// =================================================================
static char s_topic[64] = "";          // wypełniane w notifications_init()
static char s_server[64] = "https://ntfy.sh";  // można podmienić na self-hosted

// =================================================================
// FLAGI ANTYSPAMOWE
// Każdy alert wysyłany tylko raz - reset gdy stan wraca do normy
// lub gdy process jest zatrzymany
// =================================================================
static struct AlertFlags {
    bool overheat       = false;   // temperatura komory > próg
    bool meatDone       = false;   // mięso osiągnęło temp. docelową
    bool sensorError    = false;   // błąd czujnika
    bool heaterFault    = false;   // awaria grzałki
    bool doorOpen       = false;   // otwarte drzwi > 60s
    bool processStarted = false;   // informacja o starcie (reset przy każdym starcie)
    bool lowTemp        = false;   // temp komory spadła o >10°C od zadanej (problem z grzałką?)
    ProcessState lastState = ProcessState::IDLE;
    double lastMeatTemp    = -999.0;
    unsigned long doorOpenSince = 0;
} flags;

// =================================================================
// PROGI ALERTÓW
// =================================================================
static const double OVERHEAT_MARGIN    =  5.0;  // stopni powyżej tSet
static const double LOW_TEMP_DROP      = 10.0;  // stopni poniżej tSet gdy grzeje
static const double MEAT_DONE_MARGIN   =  1.0;  // stopni do tMeatTarget → alert
static const unsigned long DOOR_ALERT_DELAY_MS = 60000; // 60s otwarcia → alert

// =================================================================
// POMOCNICZE: wyślij POST do ntfy.sh
// =================================================================
static bool ntfy_post(const char* title, const char* message,
                      int priority, const char* tags) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // [FIX-N2] Nie wysylaj jesli topic pusty lub powiadomienia wylaczone
    if (s_topic[0] == '\0') return false;
    if (!storage_get_ntfy_enabled()) return false;

    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "%s/%s", s_server, s_topic);

    http.begin(url);
    http.addHeader("Content-Type",  "text/plain; charset=utf-8");
    http.addHeader("Title",         title);
    http.addHeader("Priority",      String(priority).c_str());
    http.addHeader("Tags",          tags);
    http.setTimeout(3000);  // [FIX-N1] max 3s - taskMonitor WDT=10s

    int code = http.POST(message);
    http.end();

    if (code == 200 || code == 204) {
        LOG_FMT(LOG_LEVEL_INFO, "[NTFY] Sent: %s (HTTP %d)", title, code);
        return true;
    } else {
        LOG_FMT(LOG_LEVEL_WARN, "[NTFY] Failed: HTTP %d", code);
        return false;
    }
}

// =================================================================
// PUBLICZNE API
// =================================================================
void notifications_init() {
    // Wygeneruj unikalny topic z MAC adresu
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_topic, sizeof(s_topic),
             "wedzarnia-%02x%02x%02x%02x",
             mac[2], mac[3], mac[4], mac[5]);

    // Sprawdź czy użytkownik nie ustawił własnego topicu w NVS
    const char* saved = storage_get_ntfy_topic();
    if (saved && strlen(saved) > 0) {
        strncpy(s_topic, saved, sizeof(s_topic) - 1);
    }

    // Sprawdź czy nie ustawił własnego serwera
    const char* savedServer = storage_get_ntfy_server();
    if (savedServer && strlen(savedServer) > 4) {
        strncpy(s_server, savedServer, sizeof(s_server) - 1);
    }

    Serial.printf("[NTFY] Topic: %s/%s\n", s_server, s_topic);
    Serial.printf("[NTFY] Subskrybuj topic '%s' w apce ntfy\n", s_topic);
}

void notify_send(const char* title, const char* message,
                 int priority, const char* tags) {
    ntfy_post(title, message, priority, tags);
}

void notifications_reset_flags() {
    flags.overheat    = false;
    flags.meatDone    = false;
    flags.sensorError = false;
    flags.heaterFault = false;
    flags.doorOpen    = false;
    flags.lowTemp     = false;
    flags.processStarted = false;
    flags.doorOpenSince  = 0;
}

// =================================================================
// MONITOR ALERTÓW - wywołuj co ~5s z taskMonitor
// =================================================================
void notifications_check() {
    // Sprawdź czy powiadomienia są włączone
    if (!storage_get_ntfy_enabled()) return;

    // Pobierz stan (krótki lock)
    ProcessState state;
    double tChamber, tMeat, tSet, tMeatTarget;
    bool doorOpen, errorSensor, errorHeater;
    int currentStep, stepCount;

    if (!state_lock(500)) return;
    state       = g_currentState;
    tChamber    = g_tChamber;
    tMeat       = g_tMeat;
    tSet        = g_tSet;
    doorOpen    = g_doorOpen;
    errorSensor = g_errorSensor;
    errorHeater = (state == ProcessState::PAUSE_HEATER_FAULT);
    currentStep = g_currentStep;
    stepCount   = g_stepCount;
    // W trybie AUTO - z aktualnego kroku profilu
    // W trybie MANUAL - g_tSet jest jednoczesnie celem komory i mięsa
    if (state == ProcessState::RUNNING_AUTO && g_currentStep < g_stepCount) {
        tMeatTarget = g_profile[g_currentStep].tMeatTarget;
    } else if (state == ProcessState::RUNNING_MANUAL) {
        tMeatTarget = tSet;  // w MANUAL: alert gdy mieso osiagnie temperature zadana
    } else {
        tMeatTarget = 0.0;
    }
    state_unlock();

    // -- 1. ZMIANA STANU: Start / Stop procesu -----------------
    if (state != flags.lastState) {
        // Start AUTO
        if (state == ProcessState::RUNNING_AUTO && !flags.processStarted) {
            flags.processStarted = true;
            char msg[128];
            snprintf(msg, sizeof(msg),
                "Zadana: %.0f°C | Profil: %s",
                tSet, storage_get_profile_path());
            ntfy_post("▶️ Proces AUTO start", msg, 3, "white_check_mark");
        }
        // Start MANUAL
        else if (state == ProcessState::RUNNING_MANUAL && !flags.processStarted) {
            flags.processStarted = true;
            char msg[64];
            snprintf(msg, sizeof(msg), "Tryb manualny | Zadana: %.0f°C", tSet);
            ntfy_post("🎮 Tryb manualny start", msg, 3, "white_check_mark");
        }
        // Stop / IDLE
        else if (state == ProcessState::IDLE &&
                 (flags.lastState == ProcessState::RUNNING_AUTO ||
                  flags.lastState == ProcessState::RUNNING_MANUAL)) {
            ntfy_post("⏹️ Proces zakończony", "Wędzarnia zatrzymana.", 3, "checkered_flag");
            notifications_reset_flags();
        }
        flags.lastState = state;
    }

    // Reszta alertów tylko gdy proces trwa
    bool running = (state == ProcessState::RUNNING_AUTO ||
                    state == ProcessState::RUNNING_MANUAL);
    if (!running) return;

    // -- 2. PRZEGRZANIE -----------------------------------------
    if (tChamber > tSet + OVERHEAT_MARGIN) {
        if (!flags.overheat) {
            flags.overheat = true;
            char msg[96];
            snprintf(msg, sizeof(msg),
                "Komora: %.1f°C | Zadana: %.0f°C | Różnica: +%.1f°C",
                tChamber, tSet, tChamber - tSet);
            ntfy_post("🔥 PRZEGRZANIE", msg, 5, "rotating_light,thermometer");
        }
    } else if (tChamber < tSet + OVERHEAT_MARGIN - 2.0) {
        flags.overheat = false;  // reset z histerezą 2°C
    }

    // -- 3. TEMPERATURA MIĘSA OSIĄGNIĘTA ------------------------
    if (tMeatTarget > 20.0 && tMeat >= tMeatTarget - MEAT_DONE_MARGIN) {
        if (!flags.meatDone) {
            flags.meatDone = true;
            char msg[96];
            snprintf(msg, sizeof(msg),
                "Mięso: %.1f°C | Cel: %.0f°C ✓",
                tMeat, tMeatTarget);
            ntfy_post("🍖 Mięso gotowe!", msg, 5, "meat_on_bone,tada");
        }
    } else if (tMeat < tMeatTarget - MEAT_DONE_MARGIN - 2.0) {
        flags.meatDone = false;
    }

    // -- 4. BŁĄD CZUJNIKA --------------------------------------
    if (errorSensor && !flags.sensorError) {
        flags.sensorError = true;
        ntfy_post("⚠️ Błąd czujnika", "Czujnik temperatury odłączony lub uszkodzony!", 4, "warning");
    } else if (!errorSensor) {
        flags.sensorError = false;
    }

    // -- 5. AWARIA GRZAŁKI -------------------------------------
    if (errorHeater && !flags.heaterFault) {
        flags.heaterFault = true;
        char msg[96];
        snprintf(msg, sizeof(msg),
            "Komora: %.1f°C | Zadana: %.0f°C | Brak wzrostu temperatury!",
            tChamber, tSet);
        ntfy_post("🚨 AWARIA GRZAŁKI", msg, 5, "rotating_light,fire_extinguisher");
    } else if (!errorHeater) {
        flags.heaterFault = false;
    }

    // -- 6. OTWARTE DRZWI > 60s --------------------------------
    if (doorOpen) {
        if (flags.doorOpenSince == 0) {
            flags.doorOpenSince = millis();
        } else if (!flags.doorOpen &&
                   millis() - flags.doorOpenSince > DOOR_ALERT_DELAY_MS) {
            flags.doorOpen = true;
            ntfy_post("🚪 Drzwi otwarte", "Drzwi wędzarni otwarte przez ponad 60 sekund!", 4, "door");
        }
    } else {
        flags.doorOpenSince = 0;
        flags.doorOpen = false;
    }

    // -- 7. PAUZA / AWARIA (zmiana stanu procesu) --------------
    if (state == ProcessState::PAUSE_OVERHEAT &&
        flags.lastState != ProcessState::PAUSE_OVERHEAT) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Komora: %.1f°C | Zadana: %.0f°C", tChamber, tSet);
        ntfy_post("⚠️ Pauza: przegrzanie", msg, 5, "rotating_light");
    }
    if (state == ProcessState::PAUSE_SENSOR &&
        flags.lastState != ProcessState::PAUSE_SENSOR) {
        ntfy_post("⚠️ Pauza: czujnik", "Utracono sygnał czujnika temperatury.", 5, "warning");
    }

    // Zaktualizuj ostatni stan (dla wykrywania zmian pauzy)
    flags.lastState = state;
}