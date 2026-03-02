// hardware.h - v2.1
// [FIX] Eksport g_spiMutex i hardware_init_spi_mutex()
#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>

// ======================================================
// GLOBALNY MUTEX SPI – współdzielony przez TFT i Flash
// Wszystkie operacje SPI (rysowanie TFT + zapis/odczyt Flash)
// muszą być chronione tym mutexem.
// ======================================================
extern SemaphoreHandle_t g_spiMutex;

// Inicjalizacja mutexa – wywołaj JAKO PIERWSZY przed init_display i init_flash
void hardware_init_spi_mutex();

// Wrappery do blokowania SPI z poziomu ui.cpp / tasks
void display_begin_transaction();
void display_end_transaction();

// Podstawowe inicjalizacje
void hardware_init_pins();
void hardware_init_ledc();
void hardware_init_sensors();
void hardware_init_display();
void hardware_init_flash();
void nvs_init();
void hardware_init_wifi();

// Diagnostyka
void initLoggingSystem();
void logToFile(const String& message);
void runStartupSelfTest();
void testOutput(int pin, const char* name);
void testButton(int pin, const char* name);
void deleteOldestLog();
