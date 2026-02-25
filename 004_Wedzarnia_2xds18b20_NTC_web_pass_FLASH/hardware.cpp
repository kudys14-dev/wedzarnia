// hardware.cpp - v2.1
// [FIX] Globalny SPI mutex współdzielony przez TFT i Flash.
//       Mutex tworzony PRZED hardware_init_display() i flash_init().
//       Bez tego: taskUI (TFT) + taskWeb (Flash) crashują SPI bus.

#include "hardware.h"
#include "config.h"
#include "state.h"
#include "outputs.h"
#include "wifimanager.h"
#include "flash_storage.h"
#include <nvs_flash.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <SPI.h>

// ======================================================
// GLOBALNY MUTEX SPI – współdzielony przez TFT i Flash
// ======================================================
SemaphoreHandle_t g_spiMutex = NULL;

// Pomocnicze makra do blokowania SPI z zewnątrz
// (używane przez Adafruit_ST7735 wrapper poniżej)
#define SPI_TAKE() (g_spiMutex ? (xSemaphoreTakeRecursive(g_spiMutex, pdMS_TO_TICKS(2000)) == pdTRUE) : true)
#define SPI_GIVE() do { if (g_spiMutex) xSemaphoreGiveRecursive(g_spiMutex); } while(0)

// ======================================================
// MUTEX SPI – funkcje dla modułów zewnętrznych (TFT wrapper)
// ======================================================
void spi_mutex_take() { SPI_TAKE(); }
void spi_mutex_give() { SPI_GIVE(); }

// ======================================================
// INICJALIZACJA PINÓW
// ======================================================
void hardware_init_pins() {
    pinMode(PIN_SSR1,      OUTPUT);
    pinMode(PIN_SSR2,      OUTPUT);
    pinMode(PIN_SSR3,      OUTPUT);
    pinMode(PIN_FAN,       OUTPUT);
    pinMode(PIN_SMOKE_FAN, OUTPUT);
    pinMode(PIN_BUZZER,    OUTPUT);

    pinMode(PIN_DOOR,      INPUT_PULLUP);
    pinMode(PIN_BTN_UP,    INPUT_PULLUP);
    pinMode(PIN_BTN_DOWN,  INPUT_PULLUP);
    pinMode(PIN_BTN_ENTER, INPUT_PULLUP);
    pinMode(PIN_BTN_EXIT,  INPUT_PULLUP);

    // CS piny zawsze HIGH na starcie
    pinMode(TFT_CS,      OUTPUT); digitalWrite(TFT_CS,      HIGH);
    pinMode(FLASH_CS_PIN, OUTPUT); digitalWrite(FLASH_CS_PIN, HIGH);

    analogReadResolution(12);
    LOG_FMT(LOG_LEVEL_INFO, "NTC pin GPIO%d configured", PIN_NTC);
    log_msg(LOG_LEVEL_INFO, "GPIO pins initialized");
}

// ======================================================
// TWORZENIE MUTEXA SPI – wywołaj PRZED init_display i init_flash
// ======================================================
void hardware_init_spi_mutex() {
    if (g_spiMutex == NULL) {
        // REKURENCYJNY mutex – konieczny gdy flash_storage woła spi_take/give
        // wielokrotnie w jednej ścieżce wywołań (np. fat_write_to_sector
        // woła _flash_write_data_locked który zwalnia i bierze mutex co stronę,
        // a cała operacja jest już wewnątrz fat_save który nie bierze mutexa).
        // Zwykły mutex deadlockowałby przy ponownym wzięciu z tego samego taska.
        g_spiMutex = xSemaphoreCreateRecursiveMutex();
        if (g_spiMutex) {
            log_msg(LOG_LEVEL_INFO, "SPI recursive mutex created");
        } else {
            log_msg(LOG_LEVEL_ERROR, "SPI mutex creation FAILED!");
        }
    }
}

void hardware_init_ledc() {
    bool ok = true;
    if (!ledcAttach(PIN_SSR1,      LEDC_FREQ, LEDC_RESOLUTION)) { log_msg(LOG_LEVEL_ERROR, "LEDC SSR1 fail");  ok = false; }
    if (!ledcAttach(PIN_SSR2,      LEDC_FREQ, LEDC_RESOLUTION)) { log_msg(LOG_LEVEL_ERROR, "LEDC SSR2 fail");  ok = false; }
    if (!ledcAttach(PIN_SSR3,      LEDC_FREQ, LEDC_RESOLUTION)) { log_msg(LOG_LEVEL_ERROR, "LEDC SSR3 fail");  ok = false; }
    if (!ledcAttach(PIN_SMOKE_FAN, LEDC_FREQ, LEDC_RESOLUTION)) { log_msg(LOG_LEVEL_ERROR, "LEDC SMOKE fail"); ok = false; }
    allOutputsOff();
    if (ok) log_msg(LOG_LEVEL_INFO, "LEDC/PWM initialized");
}

void hardware_init_sensors() {
    sensors.begin();
    sensors.setWaitForConversion(false);
    sensors.setResolution(12);
    LOG_FMT(LOG_LEVEL_INFO, "Found %d DS18B20 sensor(s)", sensors.getDeviceCount());
}

// ======================================================
// INICJALIZACJA WYŚWIETLACZA – bierze mutex SPI
// ======================================================
void hardware_init_display() {
    // Inicjalizacja TFT może zajmować chwilę – bierzemy mutex
    // Nie używamy spi_mutex_take() bo mutex może być jeszcze NULL (setup jest single-thread)
    // W setup() nie ma innych tasków więc mutex nie jest potrzebny podczas init

    SPI.begin(18, 19, 23, TFT_CS);
    delay(10);

    display.initR(INITR_BLACKTAB);
    display.setRotation(0);
    display.fillScreen(ST77XX_BLACK);
    display.setCursor(10, 20);
    display.setTextColor(ST77XX_WHITE);
    display.setTextSize(2);
    display.println("WEDZARNIA");
    display.setTextSize(1);
    display.println("\n   " FW_VERSION);
    display.println("   by " FW_AUTHOR);
    display.println("\n   Inicjalizacja...");
    delay(1500);

    // Po powrocie z display – upewnij się że TFT_CS=HIGH i Flash_CS=HIGH
    digitalWrite(TFT_CS,       HIGH);
    digitalWrite(FLASH_CS_PIN, HIGH);

    log_msg(LOG_LEVEL_INFO, "Display initialized");
}

// ======================================================
// INICJALIZACJA FLASH W25Q128 – przekazuje g_spiMutex
// ======================================================
void hardware_init_flash() {
    esp_task_wdt_reset();

    // Upewnij się że TFT nie blokuje SPI
    digitalWrite(TFT_CS,       HIGH);
    digitalWrite(FLASH_CS_PIN, HIGH);
    delay(10);

    // Reinicjalizacja SPI z CS na pin flash (W25Q128)
    SPI.end();
    delay(10);
    SPI.begin(18, 19, 23, FLASH_CS_PIN);
    delay(10);

    log_msg(LOG_LEVEL_INFO, "Initializing W25Q128 flash...");
    LOG_FMT(LOG_LEVEL_INFO, "CS=%d MOSI=23 MISO=19 SCK=18", FLASH_CS_PIN);

    const int MAX_RETRIES = 3;
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        esp_task_wdt_reset();
        LOG_FMT(LOG_LEVEL_INFO, "Flash init attempt %d/%d", attempt, MAX_RETRIES);

        // [FIX] Przekaż g_spiMutex do flash_init()
        if (flash_init(g_spiMutex)) {
            uint16_t jedec = flash_get_jedec_id();
            LOG_FMT(LOG_LEVEL_INFO, "Flash OK: JEDEC=0x%04X, free=%lu sectors",
                    jedec, flash_get_free_sectors());

            if (!flash_dir_exists("/profiles")) {
                flash_mkdir("/profiles");
                log_msg(LOG_LEVEL_INFO, "Created /profiles marker");
            }

            initLoggingSystem();
            esp_task_wdt_reset();
            return;
        }

        LOG_FMT(LOG_LEVEL_WARN, "Flash init failed (attempt %d/%d)", attempt, MAX_RETRIES);
        if (attempt < MAX_RETRIES) {
            delay(500);
            SPI.end();
            delay(100);
            SPI.begin(18, 19, 23, FLASH_CS_PIN);
            delay(50);
        }
    }

    log_msg(LOG_LEVEL_ERROR, "W25Q128 init FAILED – manual mode only");
    if (state_lock()) { g_errorProfile = true; state_unlock(); }
    buzzerBeep(3, 200, 200);
}

// ======================================================
// WRAPPERY TFT – blokują SPI mutex przed rysowaniem
// Adafruit_ST7735 nie ma wbudowanego mutexa więc wywołuj te
// funkcje zamiast bezpośrednich metod display w taskUI
// ======================================================
void display_begin_transaction() {
    SPI_TAKE();
    digitalWrite(TFT_CS,       HIGH);  // Flash off
    // TFT_CS jest zarządzane przez bibliotekę Adafruit
}

void display_end_transaction() {
    SPI_GIVE();
}

void nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        log_msg(LOG_LEVEL_INFO, "Erasing NVS...");
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
// SYSTEM LOGÓW
// ======================================================
void initLoggingSystem() {
    if (!flash_is_ready()) return;

    char logFiles[20][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/logs/", logFiles, 20);
    if (fileCount > 10) deleteOldestLog();

    char filename[48];
    snprintf(filename, sizeof(filename), "/logs/w_%lu.log", millis() / 1000);

    String hdr = "=== WEDZARNIA LOG ===\nTS: " + String(millis() / 1000) +
                 "\nHeap: " + String(ESP.getFreeHeap()) + "\n";

    if (flash_file_write_string(filename, hdr)) {
        LOG_FMT(LOG_LEVEL_INFO, "Log created: %s", filename);
    } else {
        log_msg(LOG_LEVEL_ERROR, "Failed to create log file");
    }
}

void deleteOldestLog() {
    if (!flash_is_ready()) return;
    char logFiles[20][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/logs/", logFiles, 20);
    if (fileCount == 0) return;

    const char* oldest = logFiles[0];
    unsigned long oldestTime = ULONG_MAX;
    const char* prefix = "/logs/w_";

    for (int i = 0; i < fileCount; i++) {
        if (strncmp(logFiles[i], prefix, strlen(prefix)) == 0) {
            const char* ts  = logFiles[i] + strlen(prefix);
            const char* dot = strrchr(logFiles[i], '.');
            if (dot && dot > ts) {
                char buf[16] = {};
                int len = dot - ts;
                if (len > 0 && len < 15) {
                    strncpy(buf, ts, len);
                    unsigned long t = strtoul(buf, NULL, 10);
                    if (t < oldestTime) { oldestTime = t; oldest = logFiles[i]; }
                }
            }
        }
    }
    if (oldestTime != ULONG_MAX) flash_file_delete(oldest);
}

void logToFile(const String& message) {
    if (!flash_is_ready()) return;
    String line = "[" + String(millis() / 1000) + "] " + message + "\n";
    flash_file_append("/logs/latest.log", line);
}

// ======================================================
// TESTY STARTOWE
// ======================================================
void runStartupSelfTest() {
    log_msg(LOG_LEVEL_INFO, "=== STARTUP SELF-TEST ===");

    bool upOk    = digitalRead(PIN_BTN_UP)    == HIGH;
    bool downOk  = digitalRead(PIN_BTN_DOWN)  == HIGH;
    bool enterOk = digitalRead(PIN_BTN_ENTER) == HIGH;
    bool exitOk  = digitalRead(PIN_BTN_EXIT)  == HIGH;

    LOG_FMT(LOG_LEVEL_INFO, "BTN UP:%s DN:%s EN:%s EX:%s DOOR:%s",
            upOk?"OK":"STUCK", downOk?"OK":"STUCK",
            enterOk?"OK":"STUCK", exitOk?"OK":"STUCK",
            digitalRead(PIN_DOOR)?"OPEN":"CLOSED");

    buzzerBeep(1, 100, 0);
    esp_task_wdt_reset();

    log_msg(LOG_LEVEL_INFO, "=== SENSOR TEST ===");
    sensors.requestTemperatures();
    delay(1000);
    esp_task_wdt_reset();

    int cnt = sensors.getDeviceCount();
    bool s1ok = false;
    if (cnt >= 1) {
        double t = sensors.getTempCByIndex(0);
        s1ok = (t != DEVICE_DISCONNECTED_C && t != 85.0 && t > -20 && t < 100);
        LOG_FMT(LOG_LEVEL_INFO, "Sensor 0: %.1f C %s", t, s1ok?"OK":"FAIL");
    }
    if (cnt >= 2) {
        double t = sensors.getTempCByIndex(1);
        bool ok = (t != DEVICE_DISCONNECTED_C && t != 85.0 && t > -20 && t < 100);
        LOG_FMT(LOG_LEVEL_INFO, "Sensor 1: %.1f C %s", t, ok?"OK":"WARN");
    }

    log_msg(LOG_LEVEL_INFO, "=== OUTPUT TEST ===");
    testOutput(PIN_SSR1, "SSR1");
    testOutput(PIN_SSR2, "SSR2");
    testOutput(PIN_SSR3, "SSR3");
    testOutput(PIN_FAN,  "FAN");
    allOutputsOff();
    esp_task_wdt_reset();

    if (upOk && downOk && enterOk && exitOk && s1ok) {
        buzzerBeep(2, 100, 50);
    } else {
        buzzerBeep(4, 150, 100);
    }
    log_msg(LOG_LEVEL_INFO, "=== SELF-TEST DONE ===");
}

void testOutput(int pin, const char* name) {
    digitalWrite(pin, HIGH); delay(50);
    digitalWrite(pin, LOW);
    LOG_FMT(LOG_LEVEL_INFO, "%s: OK", name);
}

void testButton(int pin, const char* name) {
    LOG_FMT(LOG_LEVEL_INFO, "%s: %s", name, digitalRead(pin) ? "HIGH" : "LOW");
}