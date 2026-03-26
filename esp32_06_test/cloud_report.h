// ============================================================
// cloud_report.h — Moduł raportowania danych do chmury
// WEDZARNIA HUB v2 — multi-device z obsługą komend
// ============================================================
#ifndef CLOUD_REPORT_H
#define CLOUD_REPORT_H
#include <Arduino.h>

// ── Nowy projekt Supabase (wedzarnia-multi) ──────────────────
#define CLOUD_URL         "https://nvlrawwdutxxtslgqlyc.supabase.co/functions/v1/smoker-data"
#define CLOUD_CMD_URL     "https://nvlrawwdutxxtslgqlyc.supabase.co/functions/v1/smoker-command"
#define CLOUD_DEVICE_KEY  CFG_CLOUD_DEVICE_KEY

// ── Identyfikator urządzenia — zmień dla każdego ESP32 ───────
#define CLOUD_DEVICE_ID   CFG_CLOUD_DEVICE_ID

// ── Interwały ────────────────────────────────────────────────
#define CLOUD_REPORT_INTERVAL  5000   // telemetria co 5s
#define CLOUD_CMD_INTERVAL     2000   // sprawdzaj komendy co 2s

void cloudReportSetup();
void cloudReportLoop();

#endif