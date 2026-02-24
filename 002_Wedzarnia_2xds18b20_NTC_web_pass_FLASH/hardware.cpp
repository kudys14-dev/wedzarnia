// hardware.cpp - [MOD] W25Q128 Flash zamiast SD card
// Wszystkie operacje SD zastąpione wywołaniami flash_storage API

#include "hardware.h"
#include "config.h"
#include "state.h"
#include "outputs.h"
#include "wifimanager.h"
#include "flash_storage.h"    // [MOD] Zamiast <SD.h>
#include <nvs_flash.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <SPI.h>

void hardware_init_pins() {
    pinMode(PIN_SSR1, OUTPUT);
    pinMode(PIN_SSR2, OUTPUT);
    pinMode(PIN_SSR3, OUTPUT);
    pinMode(PIN_FAN, OUTPUT);
    pinMode(PIN_SMOKE_FAN, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);

    pinMode(PIN_DOOR, INPUT_PULLUP);
    pinMode(PIN_BTN_UP, INPUT_PULLUP);
    pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_ENTER, INPUT_PULLUP);
    pinMode(PIN_BTN_EXIT, INPUT_PULLUP);

    analogReadResolution(12);
    LOG_FMT(LOG_LEVEL_INFO, "NTC pin GPIO%d configured (analog input)", PIN_NTC);
    log_msg(LOG_LEVEL_INFO, "GPIO pins initialized");
}

void hardware_init_ledc() {
    bool success = true;

    if (!ledcAttach(PIN_SSR1, LEDC_FREQ, LEDC_RESOLUTION)) {
        log_msg(LOG_LEVEL_ERROR, "LEDC SSR1 attach failed!");
        success = false;
    }
    if (!ledcAttach(PIN_SSR2, LEDC_FREQ, LEDC_RESOLUTION)) {
        log_msg(LOG_LEVEL_ERROR, "LEDC SSR2 attach failed!");
        success = false;
    }
    if (!ledcAttach(PIN_SSR3, LEDC_FREQ, LEDC_RESOLUTION)) {
        log_msg(LOG_LEVEL_ERROR, "LEDC SSR3 attach failed!");
        success = false;
    }
    if (!ledcAttach(PIN_SMOKE_FAN, LEDC_FREQ, LEDC_RESOLUTION)) {
        log_msg(LOG_LEVEL_ERROR, "LEDC SMOKE attach failed!");
        success = false;
    }

    allOutputsOff();

    if (success) {
        log_msg(LOG_LEVEL_INFO, "LEDC/PWM initialized");
    }
}

void hardware_init_sensors() {
    sensors.begin();
    sensors.setWaitForConversion(false);
    sensors.setResolution(12);

    int deviceCount = sensors.getDeviceCount();
    LOG_FMT(LOG_LEVEL_INFO, "Found %d DS18B20 sensor(s)", deviceCount);

    if (deviceCount == 0) {
        log_msg(LOG_LEVEL_WARN, "No temperature sensors found!");
    }
}

void hardware_init_display() {
    display.initR(INITR_BLACKTAB);
    display.setRotation(0);
    display.fillScreen(ST77XX_BLACK);
    display.setCursor(10, 20);
    display.setTextColor(ST77XX_WHITE);
    display.setTextSize(2);
    display.println("WEDZARNIA");
    display.setTextSize(1);
    display.println("\n   "     FW_VERSION);
    display.println("   by " FW_AUTHOR);
    display.println("\n   Inicjalizacja...");
    delay(1500);
    log_msg(LOG_LEVEL_INFO, "Display initialized");
}

// ======================================================
// [MOD] INICJALIZACJA W25Q128 FLASH (zamiast SD card)
// ======================================================
void hardware_init_flash() {
    esp_task_wdt_reset();

    // Upewnij się że TFT nie blokuje SPI
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    delay(50);

    // Inicjalizuj SPI
    SPI.end();
    delay(10);
    SPI.begin(18, 19, 23, PIN_FLASH_CS);
    delay(10);

    log_msg(LOG_LEVEL_INFO, "Initializing W25Q128 flash...");
    LOG_FMT(LOG_LEVEL_INFO, "Flash CS pin: %d, SPI pins: MOSI=23 MISO=19 SCK=18", PIN_FLASH_CS);

    const int MAX_RETRIES = 3;

    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        esp_task_wdt_reset();
        LOG_FMT(LOG_LEVEL_INFO, "Flash init attempt %d/%d", attempt, MAX_RETRIES);

        if (flash_init()) {
            uint16_t jedecId = flash_get_jedec_id();
            uint32_t totalSize = flash_get_total_size() / (1024 * 1024);
            LOG_FMT(LOG_LEVEL_INFO, "Flash OK: JEDEC=0x%04X, %lu MB", jedecId, totalSize);

            // Sprawdź czy katalogi istnieją, jeśli nie - utwórz
            if (!flash_dir_exists("/profiles")) {
                flash_mkdir("/profiles");
                log_msg(LOG_LEVEL_INFO, "Created /profiles directory marker");
            }

            initLoggingSystem();
            return;
        }

        LOG_FMT(LOG_LEVEL_WARN, "Flash init failed (attempt %d/%d)", attempt, MAX_RETRIES);

        if (attempt < MAX_RETRIES) {
            delay(500);
            SPI.end();
            delay(100);
            SPI.begin(18, 19, 23, PIN_FLASH_CS);
            delay(50);
        }
    }

    LOG_FMT(LOG_LEVEL_ERROR, "W25Q128 init failed after %d attempts", MAX_RETRIES);
    log_msg(LOG_LEVEL_ERROR, "System will run in MANUAL MODE ONLY");

    if (state_lock()) {
        g_errorProfile = true;
        state_unlock();
    }

    buzzerBeep(3, 200, 200);
}

void nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        log_msg(LOG_LEVEL_INFO, "Erasing NVS flash...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    log_msg(LOG_LEVEL_INFO, "NVS initialized");
}

void hardware_init_wifi() {
    wifi_init();
}

// ======================================================
// [MOD] SYSTEM LOGÓW NA W25Q128 FLASH
// ======================================================
void initLoggingSystem() {
    if (!flash_is_ready()) return;

    // Sprawdź ile logów istnieje, usuń najstarsze jeśli > 10
    char logFiles[20][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/logs/", logFiles, 20);

    if (fileCount > 10) {
        deleteOldestLog();
    }

    // Utwórz nowy plik logu
    char filename[48];
    snprintf(filename, sizeof(filename), "/logs/wedzarnia_%lu.log", millis() / 1000);

    String logHeader = "=== WEDZARNIA LOG START ===\n";
    logHeader += "Timestamp: " + String(millis() / 1000) + "\n";
    logHeader += "Free heap: " + String(ESP.getFreeHeap()) + "\n";

    if (flash_file_write_string(filename, logHeader)) {
        LOG_FMT(LOG_LEVEL_INFO, "Log file created: %s", filename);
    } else {
        log_msg(LOG_LEVEL_ERROR, "Failed to create log file");
    }
}

void deleteOldestLog() {
    if (!flash_is_ready()) return;

    char logFiles[20][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/logs/", logFiles, 20);

    if (fileCount == 0) return;

    // Znajdź najstarszy (najniższy timestamp w nazwie)
    const char* oldest = logFiles[0];
    unsigned long oldestTime = ULONG_MAX;

    for (int i = 0; i < fileCount; i++) {
        const char* prefix = "/logs/wedzarnia_";
        if (strncmp(logFiles[i], prefix, strlen(prefix)) == 0) {
            const char* timeStart = logFiles[i] + strlen(prefix);
            const char* dot = strrchr(logFiles[i], '.');
            if (dot && dot > timeStart) {
                char timeStr[16];
                int timeLen = dot - timeStart;
                if (timeLen > 0 && timeLen < (int)sizeof(timeStr)) {
                    strncpy(timeStr, timeStart, timeLen);
                    timeStr[timeLen] = '\0';
                    unsigned long fileTime = strtoul(timeStr, NULL, 10);
                    if (fileTime < oldestTime) {
                        oldestTime = fileTime;
                        oldest = logFiles[i];
                    }
                }
            }
        }
    }

    if (oldestTime != ULONG_MAX) {
        if (flash_file_delete(oldest)) {
            LOG_FMT(LOG_LEVEL_INFO, "Deleted oldest log: %s", oldest);
        } else {
            LOG_FMT(LOG_LEVEL_ERROR, "Failed to delete: %s", oldest);
        }
    }
}

void logToFile(const String& message) {
    if (!flash_is_ready()) return;

    String logLine = "[" + String(millis() / 1000) + "] " + message + "\n";
    flash_file_append("/logs/latest.log", logLine);
}

// ======================================================
// TESTY STARTOWE (bez zmian w logice)
// ======================================================
void runStartupSelfTest() {
    log_msg(LOG_LEVEL_INFO, "Running startup self-test...");

    log_msg(LOG_LEVEL_INFO, "=== BUTTONS TEST (Quick Check) ===");

    bool upOk = (digitalRead(PIN_BTN_UP) == HIGH);
    bool downOk = (digitalRead(PIN_BTN_DOWN) == HIGH);
    bool enterOk = (digitalRead(PIN_BTN_ENTER) == HIGH);
    bool exitOk = (digitalRead(PIN_BTN_EXIT) == HIGH);

    LOG_FMT(LOG_LEVEL_INFO, "UP: %s", upOk ? "OK (HIGH)" : "PRESSED or ERROR");
    LOG_FMT(LOG_LEVEL_INFO, "DOWN: %s", downOk ? "OK (HIGH)" : "PRESSED or ERROR");
    LOG_FMT(LOG_LEVEL_INFO, "ENTER: %s", enterOk ? "OK (HIGH)" : "PRESSED or ERROR");
    LOG_FMT(LOG_LEVEL_INFO, "EXIT: %s", exitOk ? "OK (HIGH)" : "PRESSED or ERROR");
    LOG_FMT(LOG_LEVEL_INFO, "DOOR: %s", digitalRead(PIN_DOOR) ? "OPEN" : "CLOSED");

    bool allButtonsOk = upOk && downOk && enterOk && exitOk;

    if (!allButtonsOk) {
        log_msg(LOG_LEVEL_WARN, "WARNING: Some buttons may be stuck or have wiring issues");
    } else {
        log_msg(LOG_LEVEL_INFO, "All buttons idle (pull-up HIGH) - OK");
    }

    buzzerBeep(1, 100, 0);

    log_msg(LOG_LEVEL_INFO, "=== TEMPERATURE SENSORS TEST ===");

    sensors.requestTemperatures();
    delay(1000);

    int sensorCount = sensors.getDeviceCount();
    bool sensor1Ok = false;
    bool sensor2Ok = false;

    if (sensorCount >= 1) {
        double temp1 = sensors.getTempCByIndex(0);
        if (temp1 != DEVICE_DISCONNECTED_C && temp1 > -20 && temp1 < 100) {
            LOG_FMT(LOG_LEVEL_INFO, "Sensor 1: %.1f C - OK", temp1);
            sensor1Ok = true;
        } else {
            log_msg(LOG_LEVEL_ERROR, "Sensor 1: FAILED or invalid reading");
        }
    }

    if (sensorCount >= 2) {
        double temp2 = sensors.getTempCByIndex(1);
        if (temp2 != DEVICE_DISCONNECTED_C && temp2 > -20 && temp2 < 100) {
            LOG_FMT(LOG_LEVEL_INFO, "Sensor 2: %.1f C - OK", temp2);
            sensor2Ok = true;
        } else {
            log_msg(LOG_LEVEL_WARN, "Sensor 2: Not connected or invalid");
        }
    } else {
        log_msg(LOG_LEVEL_WARN, "Only 1 sensor detected (minimum for basic operation)");
    }

    log_msg(LOG_LEVEL_INFO, "=== OUTPUT TEST ===");

    testOutput(PIN_SSR1, "Heater 1");
    delay(50);
    testOutput(PIN_SSR2, "Heater 2");
    delay(50);
    testOutput(PIN_SSR3, "Heater 3");
    delay(50);
    testOutput(PIN_FAN, "Fan");
    delay(50);

    allOutputsOff();

    log_msg(LOG_LEVEL_INFO, "=== STARTUP SELF-TEST SUMMARY ===");
    LOG_FMT(LOG_LEVEL_INFO, "Buttons: %s", allButtonsOk ? "ALL OK" : "CHECK CONNECTIONS");
    LOG_FMT(LOG_LEVEL_INFO, "Sensors: %s (%d detected)", sensor1Ok ? "OK" : "FAILED", sensorCount);
    log_msg(LOG_LEVEL_INFO, "Outputs: TESTED");
    LOG_FMT(LOG_LEVEL_INFO, "Door: %s", digitalRead(PIN_DOOR) ? "OPEN" : "CLOSED");

    if (allButtonsOk && sensor1Ok) {
        buzzerBeep(2, 100, 50);
    } else {
        buzzerBeep(4, 150, 100);
    }

    log_msg(LOG_LEVEL_INFO, "Self-test completed");
}

void testOutput(int pin, const char* name) {
    digitalWrite(pin, HIGH);
    delay(50);
    digitalWrite(pin, LOW);
    LOG_FMT(LOG_LEVEL_INFO, "%s test: OK", name);
}

void testButton(int pin, const char* name) {
    bool state = digitalRead(pin);
    LOG_FMT(LOG_LEVEL_INFO, "%s: %s", name, state ? "HIGH" : "LOW");
}
