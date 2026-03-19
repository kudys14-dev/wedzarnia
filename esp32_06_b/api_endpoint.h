// ============================================================
// api_endpoint.h — JSON API endpoint do połączeń lokalnych (WiFi LAN)
// Dodaj do web_server.cpp w setupie serwera
// ============================================================
#ifndef API_ENDPOINT_H
#define API_ENDPOINT_H

#include <WebServer.h>

// Dodaj to do setup() serwera WWW:
// server.on("/api/status", HTTP_GET, handleApiStatus);
void handleApiStatus();

// Opcjonalnie: sterowanie z aplikacji
// server.on("/api/command", HTTP_POST, handleApiCommand);
void handleApiCommand();

void setupApiEndpoints(WebServer &server);

#endif
