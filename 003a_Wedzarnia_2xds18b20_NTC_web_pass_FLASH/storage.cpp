// storage.cpp - [MOD] W25Q128 Flash zamiast SD card
// Wszystkie operacje SD.open/SD.exists/File zastąpione flash_storage API
#include "storage.h"
#include "config.h"
#include "state.h"
#include "flash_storage.h"    // [MOD] Zamiast <SD.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static char lastProfilePath[64] = "/profiles/test.prof";
static char wifiStaSsid[32] = "";
static char wifiStaPass[64] = "";

static char authUser[32] = "";
static char authPass[64] = "";

static int backupCounter = 0;
static constexpr int MAX_BACKUPS = 5;

static bool parseBool(const char* s) {
    return (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0);
}

const char* storage_get_profile_path() { return lastProfilePath; }
const char* storage_get_wifi_ssid()    { return wifiStaSsid; }
const char* storage_get_wifi_pass()    { return wifiStaPass; }

// ======================================================
// AUTORYZACJA
// ======================================================

const char* storage_get_auth_user() {
    return (authUser[0] != '\0') ? authUser : CFG_AUTH_DEFAULT_USER;
}

const char* storage_get_auth_pass() {
    return (authPass[0] != '\0') ? authPass : CFG_AUTH_DEFAULT_PASS;
}

static bool parseProfileLine(char* line, Step& step) {
    while (*line == ' ' || *line == '\t') line++;

    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }

    if (len == 0 || line[0] == '#') return false;

    char* fields[10];
    int fieldCount = 0;
    char* token = strtok(line, ";");
    while (token && fieldCount < 10) {
        fields[fieldCount++] = token;
        token = strtok(NULL, ";");
    }

    if (fieldCount < 10) {
        log_msg(LOG_LEVEL_WARN, "Invalid profile line - not enough fields");
        return false;
    }

    strncpy(step.name, fields[0], sizeof(step.name) - 1);
    step.name[sizeof(step.name) - 1] = '\0';
    step.tSet         = constrain(atof(fields[1]), CFG_T_MIN_SET, CFG_T_MAX_SET);
    step.tMeatTarget  = constrain(atof(fields[2]), 0, 100);
    step.minTimeMs    = (unsigned long)(atoi(fields[3])) * 60UL * 1000UL;
    step.powerMode    = constrain(atoi(fields[4]), CFG_POWERMODE_MIN, CFG_POWERMODE_MAX);
    step.smokePwm     = constrain(atoi(fields[5]), CFG_SMOKE_PWM_MIN, CFG_SMOKE_PWM_MAX);
    step.fanMode      = constrain(atoi(fields[6]), 0, 2);
    step.fanOnTime    = max(1000UL, (unsigned long)(atoi(fields[7])) * 1000UL);
    step.fanOffTime   = max(1000UL, (unsigned long)(atoi(fields[8])) * 1000UL);
    step.useMeatTemp  = parseBool(fields[9]);

    return true;
}

// ======================================================
// [MOD] ŁADOWANIE PROFILU Z FLASH (zamiast SD)
// ======================================================
bool storage_load_profile() {
    if (strncmp(lastProfilePath, "github:", 7) == 0) {
        return storage_load_github_profile(lastProfilePath + 7);
    } else {
        if (!flash_file_exists(lastProfilePath)) {
            LOG_FMT(LOG_LEVEL_ERROR, "Profile not found in flash: %s", lastProfilePath);
            if (state_lock()) {
                g_errorProfile = true;
                state_unlock();
            }
            return false;
        }

        storage_backup_config();

        // [MOD] Odczyt z flash zamiast SD
        String content = flash_file_read_string(lastProfilePath);
        if (content.length() == 0) {
            log_msg(LOG_LEVEL_ERROR, "Cannot read profile file from flash");
            if (state_lock()) {
                g_errorProfile = true;
                state_unlock();
            }
            return false;
        }

        int loadedStepCount = 0;
        int pos = 0;
        int contentLen = content.length();

        while (pos < contentLen && loadedStepCount < MAX_STEPS) {
            int eol = content.indexOf('\n', pos);
            if (eol < 0) eol = contentLen;

            char lineBuf[256];
            int lineLen = min((int)(eol - pos), (int)(sizeof(lineBuf) - 1));
            content.substring(pos, pos + lineLen).toCharArray(lineBuf, sizeof(lineBuf));
            lineBuf[lineLen] = '\0';

            if (parseProfileLine(lineBuf, g_profile[loadedStepCount])) {
                loadedStepCount++;
            }
            pos = eol + 1;
        }

        if (state_lock()) {
            g_stepCount    = loadedStepCount;
            g_errorProfile = (g_stepCount == 0);

            g_processStats.totalProcessTimeSec = 0;
            for (int i = 0; i < g_stepCount; i++) {
                g_processStats.totalProcessTimeSec += g_profile[i].minTimeMs / 1000;
            }
            state_unlock();
        }

        if (g_errorProfile) {
            LOG_FMT(LOG_LEVEL_ERROR, "Failed to load profile: %s", lastProfilePath);
        } else {
            LOG_FMT(LOG_LEVEL_INFO, "Profile loaded from flash: %d steps", g_stepCount);
        }

        return !g_errorProfile;
    }
}

// ======================================================
// NVS (bez zmian - NVS nie używa SD)
// ======================================================

void storage_load_config_nvs() {
    nvs_handle_t nvsHandle;
    if (nvs_open("wedzarnia", NVS_READONLY, &nvsHandle) != ESP_OK) {
        log_msg(LOG_LEVEL_INFO, "No saved config in NVS");
        return;
    }

    size_t len;
    len = sizeof(wifiStaSsid);
    if (nvs_get_str(nvsHandle, "wifi_ssid", wifiStaSsid, &len) != ESP_OK)
        wifiStaSsid[0] = '\0';

    len = sizeof(wifiStaPass);
    if (nvs_get_str(nvsHandle, "wifi_pass", wifiStaPass, &len) != ESP_OK)
        wifiStaPass[0] = '\0';

    len = sizeof(lastProfilePath);
    if (nvs_get_str(nvsHandle, "profile", lastProfilePath, &len) != ESP_OK) {
        strcpy(lastProfilePath, "/profiles/test.prof");
    }

    len = sizeof(authUser);
    if (nvs_get_str(nvsHandle, "auth_user", authUser, &len) != ESP_OK)
        authUser[0] = '\0';

    len = sizeof(authPass);
    if (nvs_get_str(nvsHandle, "auth_pass", authPass, &len) != ESP_OK)
        authPass[0] = '\0';

    if (state_lock()) {
        double tmp_d;
        len = sizeof(tmp_d);
        if (nvs_get_blob(nvsHandle, "manual_tset", &tmp_d, &len) == ESP_OK)
            g_tSet = tmp_d;

        int32_t tmp_i;
        if (nvs_get_i32(nvsHandle, "manual_pow", &tmp_i) == ESP_OK)
            g_powerMode = tmp_i;

        if (nvs_get_i32(nvsHandle, "manual_smoke", &tmp_i) == ESP_OK)
            g_manualSmokePwm = tmp_i;

        if (nvs_get_i32(nvsHandle, "manual_fan", &tmp_i) == ESP_OK)
            g_fanMode = tmp_i;

        state_unlock();
    }

    nvs_close(nvsHandle);
    log_msg(LOG_LEVEL_INFO, "NVS config loaded");
}

static void nvs_save_generic(std::function<void(nvs_handle_t)> action) {
    nvs_handle_t nvsHandle;
    if (nvs_open("wedzarnia", NVS_READWRITE, &nvsHandle) == ESP_OK) {
        action(nvsHandle);
        nvs_commit(nvsHandle);
        nvs_close(nvsHandle);
    }
}

void storage_save_wifi_nvs(const char* ssid, const char* pass) {
    strncpy(wifiStaSsid, ssid, sizeof(wifiStaSsid) - 1);
    wifiStaSsid[sizeof(wifiStaSsid) - 1] = '\0';
    strncpy(wifiStaPass, pass, sizeof(wifiStaPass) - 1);
    wifiStaPass[sizeof(wifiStaPass) - 1] = '\0';

    nvs_save_generic([&](nvs_handle_t handle){
        nvs_set_str(handle, "wifi_ssid", wifiStaSsid);
        nvs_set_str(handle, "wifi_pass", wifiStaPass);
    });

    log_msg(LOG_LEVEL_INFO, "WiFi credentials saved to NVS");
}

void storage_save_profile_path_nvs(const char* path) {
    strncpy(lastProfilePath, path, sizeof(lastProfilePath) - 1);
    lastProfilePath[sizeof(lastProfilePath) - 1] = '\0';

    nvs_save_generic([&](nvs_handle_t handle){
        nvs_set_str(handle, "profile", lastProfilePath);
    });

    LOG_FMT(LOG_LEVEL_INFO, "Profile path saved: %s", path);
}

void storage_save_manual_settings_nvs() {
    if (!state_lock()) return;

    double ts = g_tSet;
    int pm    = g_powerMode;
    int sm    = g_manualSmokePwm;
    int fm    = g_fanMode;
    state_unlock();

    nvs_save_generic([=](nvs_handle_t handle){
        nvs_set_blob(handle, "manual_tset", &ts, sizeof(ts));
        nvs_set_i32(handle, "manual_pow",   pm);
        nvs_set_i32(handle, "manual_smoke", sm);
        nvs_set_i32(handle, "manual_fan",   fm);
    });

    log_msg(LOG_LEVEL_DEBUG, "Manual settings saved to NVS");
}

void storage_save_auth_nvs(const char* user, const char* pass) {
    if (strlen(user) == 0 || strlen(user) >= sizeof(authUser) ||
        strlen(pass) == 0 || strlen(pass) >= sizeof(authPass)) {
        log_msg(LOG_LEVEL_ERROR, "Auth save failed: invalid user/pass length");
        return;
    }

    strncpy(authUser, user, sizeof(authUser) - 1);
    authUser[sizeof(authUser) - 1] = '\0';
    strncpy(authPass, pass, sizeof(authPass) - 1);
    authPass[sizeof(authPass) - 1] = '\0';

    nvs_save_generic([&](nvs_handle_t handle){
        nvs_set_str(handle, "auth_user", authUser);
        nvs_set_str(handle, "auth_pass", authPass);
    });

    log_msg(LOG_LEVEL_INFO, "Auth credentials saved to NVS");
}

void storage_reset_auth_nvs() {
    authUser[0] = '\0';
    authPass[0] = '\0';

    nvs_handle_t nvsHandle;
    if (nvs_open("wedzarnia", NVS_READWRITE, &nvsHandle) == ESP_OK) {
        nvs_erase_key(nvsHandle, "auth_user");
        nvs_erase_key(nvsHandle, "auth_pass");
        nvs_commit(nvsHandle);
        nvs_close(nvsHandle);
    }

    LOG_FMT(LOG_LEVEL_INFO, "Auth reset to defaults (user=%s)", CFG_AUTH_DEFAULT_USER);
}

// ======================================================
// [MOD] LISTOWANIE PROFILI Z FLASH (zamiast SD)
// ======================================================
String storage_list_profiles_json() {
    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");

    char files[50][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/profiles/", files, 50);

    bool first = true;
    for (int i = 0; i < fileCount; i++) {
        const char* fileName = files[i];
        // Wyciągnij samą nazwę pliku (bez ścieżki /profiles/)
        const char* baseName = strrchr(fileName, '/');
        if (baseName) baseName++; else baseName = fileName;

        int nameLen = strlen(baseName);
        if (nameLen > 5 && strcmp(baseName + nameLen - 5, ".prof") == 0) {
            if (!first) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",");
            }
            offset += snprintf(json + offset, sizeof(json) - offset, "\"%s\"", baseName);
            first = false;
            if (offset >= (int)sizeof(json) - 20) break;
        }
    }
    snprintf(json + offset, sizeof(json) - offset, "]");

    return String(json);
}

// ======================================================
// [MOD] REINICJALIZACJA FLASH (zamiast SD)
// ======================================================
bool storage_reinit_flash() {
    log_msg(LOG_LEVEL_INFO, "Re-initializing W25Q128 flash...");

    if (flash_init()) {
        log_msg(LOG_LEVEL_INFO, "Flash re-initialized successfully");
        return true;
    } else {
        log_msg(LOG_LEVEL_ERROR, "Failed to re-initialize flash");
        return false;
    }
}

// ======================================================
// [MOD] ODCZYT PROFILU JAKO JSON Z FLASH
// ======================================================
String storage_get_profile_as_json(const char* profileName) {
    char path[96];
    snprintf(path, sizeof(path), "/profiles/%s", profileName);

    if (!flash_file_exists(path)) {
        LOG_FMT(LOG_LEVEL_WARN, "Profile not found: %s", path);
        return "[]";
    }

    String content = flash_file_read_string(path);
    if (content.length() == 0) {
        LOG_FMT(LOG_LEVEL_ERROR, "Cannot read profile: %s", path);
        return "[]";
    }

    char json[2048];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    bool firstStep = true;

    int pos = 0;
    int contentLen = content.length();

    while (pos < contentLen) {
        int eol = content.indexOf('\n', pos);
        if (eol < 0) eol = contentLen;

        char lineBuf[256];
        int lineLen = min((int)(eol - pos), (int)(sizeof(lineBuf) - 1));
        content.substring(pos, pos + lineLen).toCharArray(lineBuf, sizeof(lineBuf));
        lineBuf[lineLen] = '\0';
        pos = eol + 1;

        char* line = lineBuf;
        while (*line == ' ' || *line == '\t') line++;
        int len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') continue;

        char lineCopy[256];
        strncpy(lineCopy, line, sizeof(lineCopy));
        lineCopy[sizeof(lineCopy) - 1] = '\0';

        char* fields[10];
        int fieldCount = 0;
        char* token = strtok(lineCopy, ";");
        while (token && fieldCount < 10) {
            fields[fieldCount++] = token;
            token = strtok(NULL, ";");
        }
        if (fieldCount < 10) continue;

        if (!firstStep) {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }
        offset += snprintf(json + offset, sizeof(json) - offset,
            "{\"name\":\"%s\",\"tSet\":%s,\"tMeat\":%s,\"minTime\":%s,"
            "\"powerMode\":%s,\"smoke\":%s,\"fanMode\":%s,"
            "\"fanOn\":%s,\"fanOff\":%s,\"useMeatTemp\":%s}",
            fields[0], fields[1], fields[2], fields[3],
            fields[4], fields[5], fields[6],
            fields[7], fields[8], fields[9]);
        firstStep = false;

        if (offset >= (int)sizeof(json) - 50) break;
    }
    snprintf(json + offset, sizeof(json) - offset, "]");

    return String(json);
}

// ======================================================
// GITHUB (bez zmian - nie używa SD)
// ======================================================
String storage_list_github_profiles_json() {
    if (WiFi.status() != WL_CONNECTED) {
        log_msg(LOG_LEVEL_WARN, "WiFi not connected - cannot list GitHub profiles");
        return "[\"Brak WiFi\"]";
    }

    HTTPClient http;
    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    http.begin(secureClient, CFG_GITHUB_API_URL);
    http.addHeader("User-Agent", "ESP32-Wedzarnia/4.0");
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        LOG_FMT(LOG_LEVEL_ERROR, "GitHub API list error: %d", httpCode);
        http.end();
        return "[\"Blad API GitHub\"]";
    }

    String responseBody = http.getString();
    http.end();

    StaticJsonDocument<4096> doc;
    DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
        LOG_FMT(LOG_LEVEL_ERROR, "GitHub JSON parse error: %s", error.c_str());
        return "[\"Blad parsowania\"]";
    }

    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    bool first = true;

    for (JsonVariant value : doc.as<JsonArray>()) {
        const char* filename = value["name"];
        if (!filename) continue;
        int nameLen = strlen(filename);
        if (nameLen > 5 && strcmp(filename + nameLen - 5, ".prof") == 0) {
            if (!first) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",");
            }
            offset += snprintf(json + offset, sizeof(json) - offset, "\"%s\"", filename);
            first = false;
            if (offset >= (int)sizeof(json) - 50) break;
        }
    }
    snprintf(json + offset, sizeof(json) - offset, "]");

    return String(json);
}

bool storage_load_github_profile(const char* profileName) {
    if (WiFi.status() != WL_CONNECTED) {
        LOG_FMT(LOG_LEVEL_ERROR, "WiFi not connected - cannot load from GitHub");
        if (state_lock()) { g_errorProfile = true; state_unlock(); }
        return false;
    }

    HTTPClient http;
    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    char url[192];
    snprintf(url, sizeof(url), "%s%s", CFG_GITHUB_PROFILES_BASE_URL, profileName);
    LOG_FMT(LOG_LEVEL_INFO, "Loading GitHub profile: %s", url);

    http.begin(secureClient, url);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("User-Agent", "ESP32-Wedzarnia/4.0");
    http.addHeader("Accept",     "text/plain");

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        LOG_FMT(LOG_LEVEL_ERROR, "GitHub GET failed HTTP %d: %s", httpCode, url);
        http.end();
        if (state_lock()) { g_errorProfile = true; state_unlock(); }
        return false;
    }

    String body = http.getString();
    http.end();

    if (body.length() == 0) {
        LOG_FMT(LOG_LEVEL_ERROR, "Empty response from GitHub for: %s", profileName);
        if (state_lock()) { g_errorProfile = true; state_unlock(); }
        return false;
    }

    LOG_FMT(LOG_LEVEL_DEBUG, "GitHub body: %d bytes", body.length());

    int loadedStepCount = 0;
    int pos = 0;
    int bodyLen = body.length();

    while (pos < bodyLen && loadedStepCount < MAX_STEPS) {
        int eol = body.indexOf('\n', pos);
        if (eol < 0) eol = bodyLen;

        char lineBuf[256];
        int lineLen = min((int)(eol - pos), (int)(sizeof(lineBuf) - 1));
        body.substring(pos, pos + lineLen).toCharArray(lineBuf, sizeof(lineBuf));
        lineBuf[lineLen] = '\0';

        if (parseProfileLine(lineBuf, g_profile[loadedStepCount])) {
            loadedStepCount++;
        }
        pos = eol + 1;
    }

    if (state_lock()) {
        g_stepCount    = loadedStepCount;
        g_errorProfile = (g_stepCount == 0);

        g_processStats.totalProcessTimeSec = 0;
        for (int i = 0; i < g_stepCount; i++) {
            g_processStats.totalProcessTimeSec += g_profile[i].minTimeMs / 1000;
        }
        state_unlock();
    }

    if (g_errorProfile) {
        LOG_FMT(LOG_LEVEL_ERROR, "No valid steps in GitHub profile: %s", profileName);
    } else {
        LOG_FMT(LOG_LEVEL_INFO, "GitHub profile '%s' OK: %d steps", profileName, g_stepCount);
    }

    return !g_errorProfile;
}

// ======================================================
// [MOD] BACKUP NA FLASH (zamiast SD)
// ======================================================
void storage_backup_config() {
    backupCounter++;
    if (backupCounter % 5 != 0) return;

    char backupPath[64];
    snprintf(backupPath, sizeof(backupPath), "/backup/config_%lu.bak", millis() / 1000);

    StaticJsonDocument<512> doc;
    doc["profile_path"]      = lastProfilePath;
    doc["wifi_ssid"]         = wifiStaSsid;
    doc["backup_timestamp"]  = millis() / 1000;

    String jsonStr;
    serializeJson(doc, jsonStr);

    if (flash_file_write_string(backupPath, jsonStr)) {
        LOG_FMT(LOG_LEVEL_INFO, "Config backup created: %s", backupPath);
    } else {
        log_msg(LOG_LEVEL_ERROR, "Failed to create backup file");
    }

    cleanupOldBackups();
}

void cleanupOldBackups() {
    char backupFiles[MAX_BACKUPS + 5][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/backup/", backupFiles, MAX_BACKUPS + 5);

    // Filtruj tylko .bak
    char bakFiles[MAX_BACKUPS + 5][MAX_FILENAME_LEN];
    int bakCount = 0;
    for (int i = 0; i < fileCount; i++) {
        int nameLen = strlen(backupFiles[i]);
        if (nameLen > 4 && strcmp(backupFiles[i] + nameLen - 4, ".bak") == 0) {
            strncpy(bakFiles[bakCount], backupFiles[i], MAX_FILENAME_LEN - 1);
            bakFiles[bakCount][MAX_FILENAME_LEN - 1] = '\0';
            bakCount++;
        }
    }

    if (bakCount > MAX_BACKUPS) {
        // Sortuj po nazwie (zawiera timestamp)
        for (int i = 0; i < bakCount - 1; i++) {
            for (int j = 0; j < bakCount - i - 1; j++) {
                if (strcmp(bakFiles[j], bakFiles[j+1]) > 0) {
                    char temp[MAX_FILENAME_LEN];
                    strcpy(temp, bakFiles[j]);
                    strcpy(bakFiles[j], bakFiles[j+1]);
                    strcpy(bakFiles[j+1], temp);
                }
            }
        }

        int filesToDelete = bakCount - MAX_BACKUPS;
        for (int i = 0; i < filesToDelete; i++) {
            if (flash_file_delete(bakFiles[i])) {
                LOG_FMT(LOG_LEVEL_INFO, "Deleted old backup: %s", bakFiles[i]);
            }
        }
    }
}

bool storage_restore_backup(const char* backupPath) {
    if (!flash_file_exists(backupPath)) {
        LOG_FMT(LOG_LEVEL_ERROR, "Backup file not found: %s", backupPath);
        return false;
    }

    String content = flash_file_read_string(backupPath);
    if (content.length() == 0) {
        log_msg(LOG_LEVEL_ERROR, "Cannot read backup file");
        return false;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, content);

    if (error) {
        LOG_FMT(LOG_LEVEL_ERROR, "Failed to parse backup JSON: %s", error.c_str());
        return false;
    }

    const char* profilePath = doc["profile_path"];
    const char* wifiSsid    = doc["wifi_ssid"];
    unsigned long timestamp = doc["backup_timestamp"];

    if (profilePath) {
        strncpy(lastProfilePath, profilePath, sizeof(lastProfilePath) - 1);
        LOG_FMT(LOG_LEVEL_INFO, "Restored profile path: %s", profilePath);
    }

    if (wifiSsid && strlen(wifiSsid) > 0) {
        strncpy(wifiStaSsid, wifiSsid, sizeof(wifiStaSsid) - 1);
        LOG_FMT(LOG_LEVEL_INFO, "Restored WiFi SSID: %s", wifiSsid);
    }

    storage_save_profile_path_nvs(lastProfilePath);
    storage_save_wifi_nvs(wifiStaSsid, wifiStaPass);

    LOG_FMT(LOG_LEVEL_INFO, "Backup restored (timestamp: %lu)", timestamp);
    return true;
}

String storage_list_backups_json() {
    char json[512];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "[");

    char files[20][MAX_FILENAME_LEN];
    int fileCount = flash_list_files("/backup/", files, 20);

    bool first = true;
    for (int i = 0; i < fileCount; i++) {
        int nameLen = strlen(files[i]);
        if (nameLen > 4 && strcmp(files[i] + nameLen - 4, ".bak") == 0) {
            const char* baseName = strrchr(files[i], '/');
            if (baseName) baseName++; else baseName = files[i];

            if (!first) {
                offset += snprintf(json + offset, sizeof(json) - offset, ",");
            }
            offset += snprintf(json + offset, sizeof(json) - offset, "\"%s\"", baseName);
            first = false;
            if (offset >= (int)sizeof(json) - 50) break;
        }
    }
    snprintf(json + offset, sizeof(json) - offset, "]");

    return String(json);
}
