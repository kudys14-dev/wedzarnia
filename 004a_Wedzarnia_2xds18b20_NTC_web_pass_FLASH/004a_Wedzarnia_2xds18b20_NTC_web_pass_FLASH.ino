// 003a_Wedzarnia_2xds18b20_NTC_web_pass_FLASH.ino
// v4.1 - [FIX] SPI mutex jako pierwszy krok inicjalizacji
#include "config.h"
#include "state.h"
#include "hardware.h"
#include "storage.h"
#include "flash_storage.h"
#include "web_server.h"
#include "tasks.h"
#include "outputs.h"
#include "ui.h"
#include <esp_task_wdt.h>

void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println("\n==========================================================");
    Serial.println("     WEDZARNIA ESP32 v4.1 (W25Q128 Flash)");
    Serial.println("==========================================================\n");

    log_msg(LOG_LEVEL_INFO, "Starting initialization...");

    // 1. NVS
    nvs_init();
    esp_task_wdt_reset();

    // 2. Mutexy stanu
    init_state();
    esp_task_wdt_reset();

    // 3. GPIO
    hardware_init_pins();
    esp_task_wdt_reset();

    // 4. UI init (tylko zmienne, bez SPI)
    ui_init();
    esp_task_wdt_reset();

    // 5. PWM/LEDC
    hardware_init_ledc();
    esp_task_wdt_reset();

    // =========================================================
    // [FIX] Mutex SPI MUSI być stworzony PRZED init_display
    //       i PRZED hardware_init_flash().
    //       Inaczej TFT i Flash mogą kolidować na SPI bus.
    // =========================================================
    // 6. [NEW] Mutex SPI
    hardware_init_spi_mutex();
    esp_task_wdt_reset();

    // 7. Wyświetlacz TFT (używa SPI)
    hardware_init_display();
    esp_task_wdt_reset();

    // 8. Czujniki temperatury (OneWire – nie SPI, ok)
    hardware_init_sensors();
    esp_task_wdt_reset();

    // 9. [FIX] Flash W25Q128 – dostaje g_spiMutex przez hardware_init_flash()
    hardware_init_flash();
    esp_task_wdt_reset();

    // 10. Konfiguracja z NVS
    storage_load_config_nvs();
    esp_task_wdt_reset();

    // 11. Test startowy
    runStartupSelfTest();
    esp_task_wdt_reset();

    // 12. WiFi
    hardware_init_wifi();
    esp_task_wdt_reset();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("AP: %s\n", WiFi.softAPIP().toString().c_str());
    }

    // 13. Serwer WWW
    web_server_init();
    esp_task_wdt_reset();

    buzzerBeep(2, 100, 100);

    Serial.println("==========================================================");
    Serial.printf("Free heap:   %u B\n", ESP.getFreeHeap());
    Serial.printf("CPU freq:    %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Flash JEDEC: 0x%04X\n", flash_get_jedec_id());
    Serial.printf("Flash free:  %lu sectors\n", flash_get_free_sectors());
    Serial.printf("Sensors:     %d\n", sensors.getDeviceCount());
    Serial.println("==========================================================\n");

    log_msg(LOG_LEVEL_INFO, "Starting FreeRTOS tasks...");
    tasks_create_all();

    log_msg(LOG_LEVEL_INFO, "[SYS] Ready!");
    buzzerBeep(3, 150, 100);
}

void loop() {
    static unsigned long lastBtnDebug = 0;
    if (millis() - lastBtnDebug > 60000) {
        lastBtnDebug = millis();
        LOG_FMT(LOG_LEVEL_DEBUG,
            "BTN UP=%d DN=%d EN=%d EX=%d DOOR=%d",
            digitalRead(PIN_BTN_UP), digitalRead(PIN_BTN_DOWN),
            digitalRead(PIN_BTN_ENTER), digitalRead(PIN_BTN_EXIT),
            digitalRead(PIN_DOOR));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    static uint32_t loopCnt = 0;
    if (++loopCnt % 10 == 0) {
        if (ESP.getFreeHeap() < 20000) {
            log_msg(LOG_LEVEL_WARN, "Low heap: " + String(ESP.getFreeHeap()));
        }
    }
}
