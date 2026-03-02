// ============================================================
// cloud_report.h — Moduł raportowania danych do chmury (Lovable Cloud)
// Dodaj do projektu ESP32 w Arduino IDE
// ============================================================
#ifndef CLOUD_REPORT_H
#define CLOUD_REPORT_H

#include <Arduino.h>

// Konfiguracja chmury — UZUPEŁNIJ SWOIMI DANYMI
// URL edge function (znajdziesz w Lovable Cloud)
#define CLOUD_URL "https://tswomrjsqusikwjjcaks.supabase.co/functions/v1/smoker-data"

// Klucz urządzenia — ustaw taki sam w Lovable Cloud > Secrets > SMOKER_DEVICE_KEY
#define CLOUD_DEVICE_KEY "Lapukabra123!"

// Interwał wysyłania w ms (domyślnie co 5 sekund)
#define CLOUD_REPORT_INTERVAL 5000

// Identyfikator urządzenia (jeśli masz kilka wędzarni)
#define CLOUD_DEVICE_ID "default"

void cloudReportSetup();
void cloudReportLoop();

#endif
