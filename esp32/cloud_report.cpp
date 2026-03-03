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
  Serial.println("[CLOUD] Cloud reporting initialized");
  Serial.print("[CLOUD] URL: ");
  Serial.println(CLOUD_URL);
}

void cloudReportLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastCloudReport < CLOUD_REPORT_INTERVAL) return;
  lastCloudReport = millis();

  HTTPClient http;
  http.begin(CLOUD_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", CLOUD_DEVICE_KEY);
  http.setTimeout(5000);

  StaticJsonDocument<512> doc;

  doc["device_id"] = CLOUD_DEVICE_ID;
  doc["t1"]   = g_tChamber1;                       // [FIX] było state.tChamber1
  doc["t2"]   = g_tChamber2;                       // [FIX] było state.tChamber2
  doc["tm"]   = g_tMeat;                           // [FIX] było state.tMeat
  doc["ts"]   = g_tSet;                            // [FIX] było state.tSet
  doc["ps"]   = processStateStr();                 // [FIX] było state.processStateStr()
  doc["cs"]   = g_currentStep;
  doc["sc"]   = g_stepCount;
  doc["sn"]   = (g_currentStep < g_stepCount) ? g_profile[g_currentStep].name : "";
  doc["str"]  = 0;                                 // TODO: czas pozostały do końca kroku
  doc["tte"]  = (unsigned long)((millis() - g_processStartTime) / 1000);
  doc["pm"]   = g_powerMode;
  doc["fm"]   = g_fanMode;
  doc["sp"]   = g_manualSmokePwm;
  doc["do"]   = g_doorOpen;
  doc["h1"]   = false;                             // TODO: uzupełnij gdy będą zmienne g_heater*
  doc["h2"]   = false;
  doc["h3"]   = false;
  doc["fo"]   = false;
  doc["pid"]  = pidOutput;
  doc["rssi"] = WiFi.RSSI();
  doc["fw"]   = FW_VERSION;                        // [FIX] było FIRMWARE_VERSION
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
