// storage_ntfy.cpp - NVS storage dla ustawień powiadomień ntfy.sh
// Dołącz do projektu obok storage.cpp
// Klucze NVS w namespace "ntfy"
#include "storage.h"
#include <Preferences.h>

static Preferences ntfyPrefs;

// ── Gettery ────────────────────────────────────────────────────────

bool storage_get_ntfy_enabled() {
    ntfyPrefs.begin("ntfy", true);
    bool v = ntfyPrefs.getBool("enabled", false);
    ntfyPrefs.end();
    return v;
}

const char* storage_get_ntfy_topic() {
    static char buf[64];
    ntfyPrefs.begin("ntfy", true);
    ntfyPrefs.getString("topic", buf, sizeof(buf));
    ntfyPrefs.end();
    return buf;
}

const char* storage_get_ntfy_server() {
    static char buf[64];
    ntfyPrefs.begin("ntfy", true);
    ntfyPrefs.getString("server", buf, sizeof(buf));
    ntfyPrefs.end();
    return buf;
}

// ── Settery ────────────────────────────────────────────────────────

void storage_save_ntfy_enabled(bool enabled) {
    ntfyPrefs.begin("ntfy", false);
    ntfyPrefs.putBool("enabled", enabled);
    ntfyPrefs.end();
}

void storage_save_ntfy_topic(const char* topic) {
    ntfyPrefs.begin("ntfy", false);
    ntfyPrefs.putString("topic", topic);
    ntfyPrefs.end();
}

void storage_save_ntfy_server(const char* server) {
    ntfyPrefs.begin("ntfy", false);
    ntfyPrefs.putString("server", server);
    ntfyPrefs.end();
}
