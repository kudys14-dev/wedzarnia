// web_server_files.cpp
// Handlery HTTP dla menedżera plików flash (/files/*)
// Wywoływane przez registerFileManagerRoutes() z web_server.cpp

#include "web_server_files.h"
#include "web_server.h"
#include "storage.h"
#include "flash_storage.h"
#include "config.h"
#include "state.h"
#include <esp_task_wdt.h>

// Deklaracja requireAuth z web_server.cpp (static tam, więc duplikujemy logikę)
// Prościej: sprawdź auth lokalnie
static bool requireAuthLocal() {
    if (!server.authenticate(storage_get_auth_user(), storage_get_auth_pass())) {
        server.requestAuthentication(BASIC_AUTH, "Wedzarnia", "Wymagane logowanie");
        return false;
    }
    return true;
}

// ======================================================
// GET /files/list?dir=/profiles/
// Zwraca JSON: {"files":[{"name":"/profiles/boczek.prof"},...],"count":1}
// ======================================================
static void handleFilesList() {
    if (!requireAuthLocal()) return;

    String dir = server.hasArg("dir") ? server.arg("dir") : "/profiles/";

    char files[64][MAX_FILENAME_LEN];
    int count = flash_list_files(dir.c_str(), files, 64);

    // Zbuduj JSON ręcznie – unikamy heap allocation
    String json = "{\"files\":[";
    bool first = true;
    for (int i = 0; i < count; i++) {
        // Pomiń markery katalogów (.dir)
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
// GET /files/read?path=/profiles/boczek.prof
// Zwraca zawartość pliku jako text/plain
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
    if (content.length() == 0) {
        // Plik może istnieć ale być pusty – to nie błąd
        server.send(200, "text/plain", "");
        return;
    }

    server.send(200, "text/plain", content);
}

// ======================================================
// POST /files/write
// Body (application/x-www-form-urlencoded):
//   path=/profiles/boczek.prof
//   data=...zawartość...
// Zwraca JSON: {"ok":true} lub {"ok":false,"message":"..."}
// ======================================================
static void handleFilesWrite() {
    if (!requireAuthLocal()) return;

    if (!server.hasArg("path") || !server.hasArg("data")) {
        server.send(400, "application/json",
            "{\"ok\":false,\"message\":\"Brak parametrów path lub data\"}");
        return;
    }

    String path    = server.arg("path");
    String content = server.arg("data");

    // Walidacja ścieżki
    if (path.isEmpty()) {
        server.send(400, "application/json",
            "{\"ok\":false,\"message\":\"Pusta ścieżka\"}");
        return;
    }
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    // Ostrzeżenie dla dużych plików
    if (content.length() > 8192) {
        server.send(413, "application/json",
            "{\"ok\":false,\"message\":\"Plik za duży (max 8192 B)\"}");
        return;
    }

    LOG_FMT(LOG_LEVEL_INFO, "handleFilesWrite: path='%s' size=%d",
            path.c_str(), content.length());

    esp_task_wdt_reset();

    if (flash_file_write_string(path.c_str(), content)) {
        LOG_FMT(LOG_LEVEL_INFO, "File written OK: %s (%d B)",
                path.c_str(), content.length());
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        uint32_t freeSectors = flash_get_free_sectors();
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Zapis nieudany. Wolne sektory: %lu. Sprawdź serial.", freeSectors);
        LOG_FMT(LOG_LEVEL_ERROR, "handleFilesWrite FAILED: %s", path.c_str());
        server.send(500, "application/json",
            String("{\"ok\":false,\"message\":\"") + msg + "\"}");
    }

    esp_task_wdt_reset();
}

// ======================================================
// POST /files/delete?path=/profiles/boczek.prof
// Zwraca JSON: {"ok":true} lub {"ok":false,"message":"..."}
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
            "{\"ok\":false,\"message\":\"Błąd usuwania pliku\"}");
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