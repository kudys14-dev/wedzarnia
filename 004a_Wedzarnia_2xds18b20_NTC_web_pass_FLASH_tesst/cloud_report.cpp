// ============================================================
// cloud_report.cpp — Wysyłanie danych z ESP32 do Lovable Cloud
// ============================================================
#include "cloud_report.h"
#include "state.h"
#include "sensors.h"
#include "outputs.h"
#include "process.h"
#include "config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static unsigned long lastCloudReport = 0;

void cloudReportSetup() {
  // Nic specjalnego — WiFi jest już skonfigurowane w wifimanager
  Serial.println("[CLOUD] Cloud reporting initialized");
  Serial.print("[CLOUD] URL: ");
  Serial.println(CLOUD_URL);
}

void cloudReportLoop() {
  // Sprawdź czy WiFi jest połączone i czy minął interwał
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastCloudReport < CLOUD_REPORT_INTERVAL) return;
  lastCloudReport = millis();

  HTTPClient http;
  http.begin(CLOUD_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", CLOUD_DEVICE_KEY);
  http.setTimeout(5000);

  // Buduj JSON z krótkimi kluczami (oszczędność RAM i transferu)
  StaticJsonDocument<512> doc;

  doc["device_id"] = CLOUD_DEVICE_ID;
  doc["t1"]   = state.tChamber1;       // DS18B20 #1
  doc["t2"]   = state.tChamber2;       // DS18B20 #2
  doc["tm"]   = state.tMeat;           // NTC 100k
  doc["ts"]   = state.tSet;            // Temperatura zadana
  doc["ps"]   = state.processStateStr(); // IDLE, RUNNING_AUTO, etc.
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
  doc["fw"]   = FIRMWARE_VERSION;
  doc["up"]   = (unsigned long)(millis() / 1000);

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    // OK — dane wysłane
  } else {
    Serial.printf("[CLOUD] Error: HTTP %d\n", httpCode);
    String response = http.getString();
    Serial.println(response);
  }

  http.end();
}
