// hardware.h - Zmodyfikowana wersja: W25Q128 zamiast SD
#pragma once
#include <Arduino.h>

// Podstawowe funkcje inicjalizacji
void hardware_init_pins();
void hardware_init_ledc();
void hardware_init_sensors();
void hardware_init_display();
void hardware_init_flash();   // [MOD] Było hardware_init_sd()
void nvs_init();
void hardware_init_wifi();

// Funkcje diagnostyczne
void initLoggingSystem();
void logToFile(const String& message);
void runStartupSelfTest();
void testOutput(int pin, const char* name);
void testButton(int pin, const char* name);
void deleteOldestLog();       // [MOD] Bez parametru - flash nie potrzebuje ścieżki
