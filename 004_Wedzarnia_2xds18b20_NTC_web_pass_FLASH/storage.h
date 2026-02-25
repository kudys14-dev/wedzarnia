// storage.h - Zmodyfikowana wersja: W25Q128 zamiast SD
#pragma once
#include <Arduino.h>

// Podstawowe funkcje
const char* storage_get_profile_path();
const char* storage_get_wifi_ssid();
const char* storage_get_wifi_pass();
bool storage_load_profile();
void storage_load_config_nvs();
void storage_save_wifi_nvs(const char* ssid, const char* pass);
void storage_save_profile_path_nvs(const char* path);
void storage_save_manual_settings_nvs();
String storage_list_profiles_json();
bool storage_reinit_flash();          // [MOD] Było storage_reinit_sd()
String storage_get_profile_as_json(const char* profileName);

// Funkcje GitHub
String storage_list_github_profiles_json();
bool storage_load_github_profile(const char* profileName);

// Funkcje backup
void storage_backup_config();
bool storage_restore_backup(const char* backupPath);
String storage_list_backups_json();
void cleanupOldBackups();

// Autoryzacja
const char* storage_get_auth_user();
const char* storage_get_auth_pass();
void storage_save_auth_nvs(const char* user, const char* pass);
void storage_reset_auth_nvs();
