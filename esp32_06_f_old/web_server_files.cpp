// web_server_files.cpp
// Handlery HTTP dla menedżera plików flash (/files/*)
// [FIX] handleFilesWrite zwraca szczegółowy komunikat błędu zamiast ogólnego 500

#include "web_server_files.h"
#include "web_server.h"
#include "storage.h"
#include "flash_storage.h"
#include "config.h"
#include "state.h"
#include <esp_task_wdt.h>

static bool requireAuthLocal() {
    if (!server.authenticate(storage_get_auth_user(), storage_get_auth_pass())) {
        server.requestAuthentication(BASIC_AUTH, "Wedzarnia", "Wymagane logowanie");
        return false;
    }
    return true;
}

// ======================================================
// GET /files/list?dir=/profiles/
// ======================================================
static void handleFilesList() {
    if (!requireAuthLocal()) return;
    String dir = server.hasArg("dir") ? server.arg("dir") : "/profiles/";
    char files[64][MAX_FILENAME_LEN];
    int count = flash_list_files(dir.c_str(), files, 64);
    String json = "{\"files\":[";
    bool first = true;
    for (int i = 0; i < count; i++) {
        const char* fname = files[i];
        int fnlen = strlen(fname);
        if (fnlen >= 4 && strcmp(fname + fnlen - 4, ".dir") == 0) continue;
        if (!first) json += ",";
        json += "{\"name\":\"";
        json += fname;
        json += "\"}";
        first = false;
    }
    json += "],\"count\":";
    json += count;
    json += "}";
    server.send(200, "application/json", json);
}

// ======================================================
// GET /files/read?path=
// ======================================================
static void handleFilesRead() {
    if (!requireAuthLocal()) return;
    if (!server.hasArg("path")) {
        server.send(400, "text/plain", "Brak parametru path");
        return;
    }
    String path = server.arg("path");
    if (!flash_file_exists(path.c_str())) {
        server.send(404, "text/plain", "Plik nie istnieje: " + path);
        return;
    }
    String content = flash_file_read_string(path.c_str());
    server.send(200, "text/plain", content);
}

// ======================================================
// POST /files/write   (application/x-www-form-urlencoded)
// [FIX] Szczegolowy komunikat bledu zamiast ogolnego 500
// ======================================================
static void handleFilesWrite() {
    if (!requireAuthLocal()) return;

    if (!server.hasArg("path") || !server.hasArg("data")) {
        server.send(400, "application/json",
            "{\"ok\":false,\"message\":\"Brak parametrow path lub data\"}");
        return;
    }

    String path    = server.arg("path");
    String content = server.arg("data");

    if (path.isEmpty()) {
        server.send(400, "application/json",
            "{\"ok\":false,\"message\":\"Pusta sciezka\"}");
        return;
    }
    if (!path.startsWith("/")) path = "/" + path;

    if (content.length() > 32768) {
        server.send(413, "application/json",
            "{\"ok\":false,\"message\":\"Plik za duzy (max 32KB)\"}");
        return;
    }

    if (!flash_is_ready()) {
        server.send(500, "application/json",
            "{\"ok\":false,\"message\":\"Flash nie gotowy! Sprawdz polaczenie SPI W25Q128.\"}");
        return;
    }

    if (content.length() == 0) {
        server.send(400, "application/json",
            "{\"ok\":false,\"message\":\"Pusta zawartosc. POST body obciete przez limit WebServera (~4096B). Plik jest za duzy.\"}");
        return;
    }

    uint32_t freeSectors = flash_get_free_sectors();
    uint32_t sectorsNeeded = (content.length() + 4095) / 4096;

    if (freeSectors < sectorsNeeded) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "Za malo miejsca! Potrzeba %lu sekt, wolnych %lu sekt",
            sectorsNeeded, freeSectors);
        server.send(500, "application/json",
            String("{\"ok\":false,\"message\":\"") + msg + "\"}");
        return;
    }

    LOG_FMT(LOG_LEVEL_INFO, "handleFilesWrite: path='%s' size=%d free=%lu",
            path.c_str(), content.length(), freeSectors);
    esp_task_wdt_reset();

    if (flash_file_write_string(path.c_str(), content)) {
        LOG_FMT(LOG_LEVEL_INFO, "File written OK: %s (%d B)",
                path.c_str(), content.length());
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        freeSectors = flash_get_free_sectors();
        char msg[192];
        snprintf(msg, sizeof(msg),
            "flash_file_write FAILED path=%s size=%dB free=%lu sekt. Szczegoly w Serial.",
            path.c_str(), content.length(), freeSectors);
        LOG_FMT(LOG_LEVEL_ERROR, "%s", msg);
        server.send(500, "application/json",
            String("{\"ok\":false,\"message\":\"") + msg + "\"}");
    }

    esp_task_wdt_reset();
}

// ======================================================
// POST /files/delete?path=
// ======================================================
static void handleFilesDelete() {
    if (!requireAuthLocal()) return;
    if (!server.hasArg("path")) {
        server.send(400, "application/json",
            "{\"ok\":false,\"message\":\"Brak parametru path\"}");
        return;
    }
    String path = server.arg("path");
    if (!flash_file_exists(path.c_str())) {
        server.send(404, "application/json",
            "{\"ok\":false,\"message\":\"Plik nie istnieje\"}");
        return;
    }
    if (flash_file_delete(path.c_str())) {
        LOG_FMT(LOG_LEVEL_INFO, "File deleted: %s", path.c_str());
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(500, "application/json",
            "{\"ok\":false,\"message\":\"Blad usuwania pliku\"}");
    }
}

// ======================================================
// REJESTRACJA ROUTÓW
// ======================================================
void registerFileManagerRoutes() {
    server.on("/files/list",   HTTP_GET,  handleFilesList);
    server.on("/files/read",   HTTP_GET,  handleFilesRead);
    server.on("/files/write",  HTTP_POST, handleFilesWrite);
    server.on("/files/delete", HTTP_POST, handleFilesDelete);
    log_msg(LOG_LEVEL_INFO, "File manager routes registered");
}