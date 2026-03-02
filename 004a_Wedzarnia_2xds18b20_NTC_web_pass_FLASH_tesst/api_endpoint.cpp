// ============================================================
// api_endpoint.cpp — JSON API dla połączeń lokalnych (WiFi)
// Aplikacja PWA łączy się z ESP32 bezpośrednio po IP
// ============================================================
#include "api_endpoint.h"
#include "state.h"
#include "sensors.h"
#include "outputs.h"
#include "process.h"
#include "config.h"

#include <WiFi.h>
#include <ArduinoJson.h>

extern WebServer server;  // Zdefiniowany w web_server.cpp

void setupApiEndpoints(WebServer &srv) {
  // CORS — pozwól na połączenia z aplikacji PWA
  srv.on("/api/status", HTTP_GET, handleApiStatus);
  srv.on("/api/status", HTTP_OPTIONS, []() {
    srv.sendHeader("Access-Control-Allow-Origin", "*");
    srv.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    srv.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    srv.send(204);
  });
  srv.on("/api/command", HTTP_POST, handleApiCommand);
  srv.on("/api/command", HTTP_OPTIONS, []() {
    srv.sendHeader("Access-Control-Allow-Origin", "*");
    srv.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    srv.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    srv.send(204);
  });
}

void handleApiStatus() {
  StaticJsonDocument<512> doc;

  doc["t1"]   = state.tChamber1;
  doc["t2"]   = state.tChamber2;
  doc["tc"]   = (state.tChamber1 + state.tChamber2) / 2.0;
  doc["tm"]   = state.tMeat;
  doc["ts"]   = state.tSet;
  doc["ps"]   = state.processStateStr();
  doc["cs"]   = state.currentStep;
  doc["sc"]   = state.stepCount;
  doc["sn"]   = state.stepName;
  doc["str"]  = state.stepTimeRemaining;
  doc["tte"]  = state.totalTimeElapsed;
  doc["pm"]   = state.powerMode;
  doc["fm"]   = state.fanMode;
  doc["sp"]   = state.smokePwm;
  doc["do"]   = state.doorOpen;
  doc["h1"]   = state.heater1;
  doc["h2"]   = state.heater2;
  doc["h3"]   = state.heater3;
  doc["fo"]   = state.fanOn;
  doc["pid"]  = state.pidOutput;
  doc["rssi"] = WiFi.RSSI();
  doc["ssid"] = WiFi.SSID();
  doc["fw"]   = FIRMWARE_VERSION;
  doc["up"]   = (unsigned long)(millis() / 1000);
  doc["fr"]   = state.flashReady;
  doc["fu"]   = state.flashUsed;
  doc["ft"]   = state.flashTotal;

  String json;
  serializeJson(doc, json);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleApiCommand() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) {
    server.send(400, "application/json", "{\"error\":\"no cmd\"}");
    return;
  }

  // Obsługa komend
  if (strcmp(cmd, "start") == 0) {
    // processStart(); — odkomentuj gdy gotowe
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"started\"}");
  }
  else if (strcmp(cmd, "pause") == 0) {
    // processPause();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"paused\"}");
  }
  else if (strcmp(cmd, "stop") == 0) {
    // processStop();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"stopped\"}");
  }
  else if (strcmp(cmd, "set_temp") == 0) {
    float temp = doc["value"] | 0.0f;
    if (temp >= 30 && temp <= 120) {
      state.tSet = temp;
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"temp out of range\"}");
    }
  }
  else {
    server.send(400, "application/json", "{\"error\":\"unknown cmd\"}");
  }
}
