// notifications.h - Powiadomienia push przez ntfy.sh
// ESP32 wysyła HTTP POST na https://ntfy.sh/<topic>
// Apka na telefon: ntfy (Android/iOS) - subskrybuj ten sam topic
#pragma once
#include <Arduino.h>

// Inicjalizacja (wywołaj w setup() po WiFi)
void notifications_init();

// Wyślij powiadomienie (tylko gdy WiFi STA podłączone)
// priority: 1=min 2=low 3=default 4=high 5=urgent(wibracja+alarm)
void notify_send(const char* title, const char* message,
                 int priority = 3, const char* tags = "");

// Monitoruj stan i wysyłaj alerty – wywołuj co ~5s z taskMonitor
void notifications_check();

// Reset flag alertów (np. po zatrzymaniu procesu)
void notifications_reset_flags();
