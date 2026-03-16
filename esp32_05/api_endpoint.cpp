// ============================================================
// api_endpoint.cpp — JSON API dla połączeń lokalnych (WiFi)
// ============================================================
#include "api_endpoint.h"
#include "state.h"
#include "sensors.h"
#include "outputs.h"
#include "process.h"
#include "config.h"

#include <WiFi.h>
#include <ArduinoJson.h>

extern WebServer server;

void setupApiEndpoints(WebServer &srv) {
  srv.on("/api/status", HTTP_GET, handleApiStatus);
  srv.on("/api/status", HTTP_OPTIONS, [&srv]() {
    srv.sendHeader("Access-Control-Allow-Origin", "*");
    srv.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    srv.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    srv.send(204);
  });
  srv.on("/api/command", HTTP_POST, handleApiCommand);
  srv.on("/api/command", HTTP_OPTIONS, [&srv]() {
    srv.sendHeader("Access-Control-Allow-Origin", "*");
    srv.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    srv.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    srv.send(204);
  });
}

void handleApiStatus() {
  StaticJsonDocument<512> doc;

  bool h1    = (ledcRead(PIN_SSR1) > 0);
  bool h2    = (ledcRead(PIN_SSR2) > 0);
  bool h3    = (ledcRead(PIN_SSR3) > 0);
  bool fanOn = (digitalRead(PIN_FAN) == HIGH);

  doc["t1"]   = g_tChamber1;
  doc["t2"]   = g_tChamber2;
  doc["tc"]   = (g_tChamber1 + g_tChamber2) / 2.0;
  doc["tm"]   = g_tMeat;
  doc["ts"]   = g_tSet;
  doc["ps"]   = processStateStr();
  doc["cs"]   = g_currentStep;
  doc["sc"]   = g_stepCount;
  doc["sn"]   = (g_currentStep < g_stepCount) ? g_profile[g_currentStep].name : "";
  doc["str"]  = 0;
  doc["tte"]  = (unsigned long)((millis() - g_processStartTime) / 1000);
  doc["pm"]   = g_powerMode;
  doc["fm"]   = g_fanMode;
  doc["sp"]   = g_manualSmokePwm;
  doc["do"]   = g_doorOpen;
  doc["h1"]   = h1;
  doc["h2"]   = h2;
  doc["h3"]   = h3;
  doc["fo"]   = fanOn;
  doc["pid"]  = pidOutput;
  doc["rssi"] = WiFi.RSSI();
  doc["ssid"] = WiFi.SSID();
  doc["fw"]   = FW_VERSION;
  doc["up"]   = (unsigned long)(millis() / 1000);
  doc["fr"]   = false;
  doc["fu"]   = 0;
  doc["ft"]   = 0;

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

  // ============================================================
  // START — uruchom proces manualny lub auto (zależnie od profilu)
  // ============================================================
  if (strcmp(cmd, "start") == 0) {
    ProcessState st = g_currentState;
    if (st == ProcessState::IDLE || st == ProcessState::PAUSE_USER) {
      if (g_stepCount > 0) {
        process_start_auto();
        server.send(200, "application/json", "{\"ok\":true,\"msg\":\"auto started\"}");
      } else {
        process_start_manual();
        server.send(200, "application/json", "{\"ok\":true,\"msg\":\"manual started\"}");
      }
    } else {
      server.send(409, "application/json", "{\"error\":\"already running\"}");
    }
  }

  // ============================================================
  // PAUSE — wstrzymaj proces (tylko gdy działa)
  // ============================================================
  else if (strcmp(cmd, "pause") == 0) {
    ProcessState st = g_currentState;
    if (st == ProcessState::RUNNING_AUTO || st == ProcessState::RUNNING_MANUAL) {
      if (state_lock()) {
        g_currentState = ProcessState::PAUSE_USER;
        state_unlock();
      }
      allOutputsOff();
      server.send(200, "application/json", "{\"ok\":true,\"msg\":\"paused\"}");
    } else {
      server.send(409, "application/json", "{\"error\":\"not running\"}");
    }
  }

  // ============================================================
  // RESUME — wznów po pauzie
  // ============================================================
  else if (strcmp(cmd, "resume") == 0) {
    ProcessState st = g_currentState;
    if (st == ProcessState::PAUSE_USER || st == ProcessState::PAUSE_DOOR ||
        st == ProcessState::PAUSE_OVERHEAT || st == ProcessState::PAUSE_HEATER_FAULT) {
      process_resume();
      server.send(200, "application/json", "{\"ok\":true,\"msg\":\"resuming\"}");
    } else {
      server.send(409, "application/json", "{\"error\":\"not paused\"}");
    }
  }

  // ============================================================
  // STOP — zatrzymaj proces i przejdź do IDLE
  // ============================================================
  else if (strcmp(cmd, "stop") == 0) {
    if (state_lock()) {
      g_currentState = ProcessState::IDLE;
      state_unlock();
    }
    allOutputsOff();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"stopped\"}");
  }

  // ============================================================
  // NEXT_STEP — przejdź do następnego kroku (tylko AUTO)
  // ============================================================
  else if (strcmp(cmd, "next_step") == 0) {
    process_force_next_step();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"next step\"}");
  }

  // ============================================================
  // SET_TEMP — zmień temperaturę docelową (30–120°C)
  // ============================================================
  else if (strcmp(cmd, "set_temp") == 0) {
    float temp = doc["value"] | 0.0f;
    if (temp >= 30 && temp <= 120) {
      if (state_lock()) {
        g_tSet = temp;
        state_unlock();
      }
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"temp out of range 30-120\"}");
    }
  }

  // ============================================================
  // SET_POWER — tryb mocy grzałek (1=jedna, 2=dwie, 3=trzy)
  // ============================================================
  else if (strcmp(cmd, "set_power") == 0) {
    int pm = doc["value"] | 0;
    if (pm >= 1 && pm <= 3) {
      if (state_lock()) {
        g_powerMode = pm;
        state_unlock();
      }
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"power mode must be 1-3\"}");
    }
  }

  // ============================================================
  // SET_FAN — tryb wentylatora (0=off, 1=on, 2=cykliczny)
  // ============================================================
  else if (strcmp(cmd, "set_fan") == 0) {
    int fm = doc["value"] | -1;
    if (fm >= 0 && fm <= 2) {
      if (state_lock()) {
        g_fanMode = fm;
        // Opcjonalne czasy cykliczne
        if (doc.containsKey("on_ms"))  g_fanOnTime  = (unsigned long)(doc["on_ms"]  | CFG_FAN_ON_DEFAULT_MS);
        if (doc.containsKey("off_ms")) g_fanOffTime = (unsigned long)(doc["off_ms"] | CFG_FAN_OFF_DEFAULT_MS);
        state_unlock();
      }
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"fan mode must be 0-2\"}");
    }
  }

  // ============================================================
  // SET_SMOKE — PWM dymogeneratora (0–255)
  // ============================================================
  else if (strcmp(cmd, "set_smoke") == 0) {
    int pwm = doc["value"] | -1;
    if (pwm >= 0 && pwm <= 255) {
      if (state_lock()) {
        g_manualSmokePwm = pwm;
        state_unlock();
      }
      // Zastosuj natychmiast jeśli proces działa
      ProcessState st = g_currentState;
      if (st == ProcessState::RUNNING_MANUAL || st == ProcessState::RUNNING_AUTO) {
        if (output_lock()) {
          ledcWrite(PIN_SMOKE_FAN, pwm);
          output_unlock();
        }
      }
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"smoke pwm must be 0-255\"}");
    }
  }

  else {
    server.send(400, "application/json", "{\"error\":\"unknown cmd\"}");
  }
}