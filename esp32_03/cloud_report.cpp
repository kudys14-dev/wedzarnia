// ============================================================
// cloud_report.cpp — Telemetria + odbieranie komend z chmury
// WEDZARNIA HUB v2 + obsługa profili Flash + GitHub
// + poprawka: rzeczywiste czasy kroku i procesu
// ============================================================
#include "cloud_report.h"
#include "state.h"
#include "sensors.h"
#include "outputs.h"
#include "process.h"
#include "storage.h"
#include "config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static unsigned long lastCloudReport = 0;
static unsigned long lastCmdCheck    = 0;

static const char* ANON_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
  "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im52bHJhd3dkdXR4eHRzbGdxbHljIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzI2NTUzOTksImV4cCI6MjA4ODIzMTM5OX0."
  "_qr9H9TC_psywsOKk-7yNtU3vS9sy4uG9quU0a6fwsA";

// ── SETUP ────────────────────────────────────────────────────
void cloudReportSetup() {
  Serial.println("[CLOUD] Wedzarnia Hub v2 initialized");
  Serial.printf("[CLOUD] Device ID: %s\n", CFG_CLOUD_DEVICE_ID);
  Serial.printf("[CLOUD] URL: %s\n", CLOUD_URL);
}

// ── POMOCNICZE: wyciągnij nazwę pliku ze ścieżki lub github:nazwa ─
static const char* extractBaseName(const char* path) {
  const char* p = strrchr(path, '/');
  if (p) return p + 1;
  p = strrchr(path, ':');
  if (p) return p + 1;
  return path;
}

// ── POMOCNICZE: zatrzymaj proces ─────────────────────────────
static void stopProcess() {
  ProcessState st = g_currentState;
  if (st == ProcessState::RUNNING_AUTO || st == ProcessState::RUNNING_MANUAL) {
    if (state_lock()) { g_currentState = ProcessState::IDLE; state_unlock(); }
    allOutputsOff();
    delay(100);
  }
}

// ── TELEMETRIA → Supabase ────────────────────────────────────
static void sendTelemetry() {
  bool h1    = (ledcRead(PIN_SSR1) > 0);
  bool h2    = (ledcRead(PIN_SSR2) > 0);
  bool h3    = (ledcRead(PIN_SSR3) > 0);
  bool fanOn = (digitalRead(PIN_FAN) == HIGH);

  // ── Odczyt czasów pod lockiem ────────────────────────────
  unsigned long stepRemaining  = 0;
  unsigned long totalElapsed   = 0;
  unsigned long totalRemaining = 0;
  int           currentStep    = 0;
  int           stepCount      = 0;
  ProcessState  state          = ProcessState::IDLE;
  const char*   stepName       = "";

  if (state_lock()) {
    state        = g_currentState;
    currentStep  = g_currentStep;
    stepCount    = g_stepCount;
    totalElapsed = (unsigned long)((millis() - g_processStartTime) / 1000);
    totalRemaining = g_processStats.remainingProcessTimeSec;

    if (state == ProcessState::RUNNING_AUTO &&
        currentStep >= 0 && currentStep < stepCount) {
      unsigned long stepElapsed = (millis() - g_stepStartTime) / 1000;
      unsigned long stepTotal   = g_profile[currentStep].minTimeMs / 1000;
      stepRemaining = (stepTotal > stepElapsed) ? (stepTotal - stepElapsed) : 0;
      stepName = g_profile[currentStep].name;
    }
    state_unlock();
  }

  double ntcAdc = 0, ntcRes = 0;
if (state_lock()) {
    ntcAdc = g_ntcAdc;
    ntcRes = g_ntcResistance;
    state_unlock();
}

  // Lista profili z Flash
  String profilesJson = storage_list_profiles_json();

  // Aktywny profil — tylko nazwa pliku
  const char* currentProfile = storage_get_profile_path();
  const char* baseName = extractBaseName(currentProfile);

  StaticJsonDocument<896> doc;
  doc["device_id"] = CFG_CLOUD_DEVICE_ID;
  doc["t1"]        = g_tChamber1;
  doc["t2"]        = g_tChamber2;
  doc["tm"]        = g_tMeat;
  doc["ts"]        = g_tSet;
  doc["ps"]        = processStateStr();
  doc["cs"]        = currentStep;
  doc["sc"]        = stepCount;
  doc["sn"]        = stepName;
  doc["str"]       = stepRemaining;    // pozostały czas kroku (sekundy)
  doc["tte"]       = totalElapsed;     // czas od startu (sekundy)
  doc["trem"]      = totalRemaining;   // pozostały czas całego procesu (sekundy)
  doc["pm"]        = g_powerMode;
  doc["fm"]        = g_fanMode;
  doc["sp"]        = g_manualSmokePwm;
  doc["do"]        = g_doorOpen;
  doc["h1"]        = h1;
  doc["h2"]        = h2;
  doc["h3"]        = h3;
  doc["fo"]        = fanOn;
  doc["pid"]       = pidOutput;
  doc["nadc"] = ntcAdc;
  doc["nr"]   = ntcRes;
  doc["rssi"]      = WiFi.RSSI();
  doc["fw"]        = FW_VERSION;
  doc["up"]        = (unsigned long)(millis() / 1000);
  doc["profiles"]  = serialized(profilesJson);
  doc["profile"]   = baseName;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(CLOUD_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", CFG_CLOUD_DEVICE_KEY);
  http.setTimeout(5000);

  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("[CLOUD] Telemetry error: HTTP %d\n", code);
  }
  http.end();
}

// ── WYKONANIE KOMENDY ────────────────────────────────────────
static void executeCommand(const char* cmd, float value, const char* strVal, const char* cmdId) {
  Serial.printf("[CLOUD] CMD: %s val=%.1f str=%s\n", cmd, value, strVal);

  // ── Sterowanie procesem ──────────────────────────────────
  if (strcmp(cmd, "start") == 0) {
    ProcessState st = g_currentState;
    if (st == ProcessState::IDLE || st == ProcessState::PAUSE_USER) {
      if (g_stepCount > 0) process_start_auto();
      else                 process_start_manual();
    }
  }
  else if (strcmp(cmd, "pause") == 0) {
    if (g_currentState == ProcessState::RUNNING_AUTO ||
        g_currentState == ProcessState::RUNNING_MANUAL) {
      if (state_lock()) { g_currentState = ProcessState::PAUSE_USER; state_unlock(); }
      allOutputsOff();
    }
  }
  else if (strcmp(cmd, "resume") == 0) {
    process_resume();
  }
  else if (strcmp(cmd, "stop") == 0) {
    if (state_lock()) { g_currentState = ProcessState::IDLE; state_unlock(); }
    allOutputsOff();
  }
  else if (strcmp(cmd, "next_step") == 0) {
    process_force_next_step();
  }

  // ── Parametry ────────────────────────────────────────────
  else if (strcmp(cmd, "set_temp") == 0) {
    if (value >= 30 && value <= 120) {
      if (state_lock()) { g_tSet = value; state_unlock(); }
    }
  }
  else if (strcmp(cmd, "set_power") == 0) {
    int pm = (int)value;
    if (pm >= 1 && pm <= 3) {
      if (state_lock()) { g_powerMode = pm; state_unlock(); }
    }
  }
  else if (strcmp(cmd, "set_fan") == 0) {
    int fm = (int)value;
    if (fm >= 0 && fm <= 2) {
      if (state_lock()) { g_fanMode = fm; state_unlock(); }
    }
  }
  else if (strcmp(cmd, "set_smoke") == 0) {
    int pwm = (int)value;
    if (pwm >= 0 && pwm <= 255) {
      if (state_lock()) { g_manualSmokePwm = pwm; state_unlock(); }
      if (g_currentState == ProcessState::RUNNING_MANUAL ||
          g_currentState == ProcessState::RUNNING_AUTO) {
        if (output_lock()) { ledcWrite(PIN_SMOKE_FAN, pwm); output_unlock(); }
      }
    }
  }

  // ── Profile Flash ────────────────────────────────────────
  else if (strcmp(cmd, "load_profile") == 0) {
    if (strlen(strVal) > 0) {
      stopProcess();
      char profilePath[64];
      snprintf(profilePath, sizeof(profilePath), "/profiles/%s", strVal);
      storage_save_profile_path_nvs(profilePath);
      bool ok = storage_load_profile();
      Serial.printf("[CLOUD] load_profile '%s': %s (%d steps)\n",
                    strVal, ok ? "OK" : "FAIL", g_stepCount);
    }
  }
  else if (strcmp(cmd, "start_profile") == 0) {
    if (strlen(strVal) > 0) {
      stopProcess();
      char profilePath[64];
      snprintf(profilePath, sizeof(profilePath), "/profiles/%s", strVal);
      storage_save_profile_path_nvs(profilePath);
      bool ok = storage_load_profile();
      if (ok && g_stepCount > 0) {
        process_start_auto();
        Serial.printf("[CLOUD] start_profile '%s': started (%d steps)\n", strVal, g_stepCount);
      } else {
        Serial.printf("[CLOUD] start_profile '%s': FAIL\n", strVal);
      }
    }
  }

  // ── Profile GitHub ───────────────────────────────────────
  else if (strcmp(cmd, "load_github_profile") == 0) {
    if (strlen(strVal) > 0) {
      stopProcess();
      bool ok = storage_load_github_profile(strVal);
      if (ok) {
        char githubPath[64];
        snprintf(githubPath, sizeof(githubPath), "github:%s", strVal);
        storage_save_profile_path_nvs(githubPath);
      }
      Serial.printf("[CLOUD] load_github '%s': %s (%d steps)\n",
                    strVal, ok ? "OK" : "FAIL", g_stepCount);
    }
  }
  else if (strcmp(cmd, "start_github_profile") == 0) {
    if (strlen(strVal) > 0) {
      stopProcess();
      bool ok = storage_load_github_profile(strVal);
      if (ok && g_stepCount > 0) {
        char githubPath[64];
        snprintf(githubPath, sizeof(githubPath), "github:%s", strVal);
        storage_save_profile_path_nvs(githubPath);
        process_start_auto();
        Serial.printf("[CLOUD] start_github '%s': started (%d steps)\n", strVal, g_stepCount);
      } else {
        Serial.printf("[CLOUD] start_github '%s': FAIL\n", strVal);
      }
    }
  }

  // ── Potwierdź wykonanie komendy (DELETE) ─────────────────
  String url = String(CLOUD_CMD_URL) + "?device_id=" + CFG_CLOUD_DEVICE_ID + "&id=" + cmdId;
  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + ANON_KEY);
  http.sendRequest("DELETE");
  http.end();
}

// ── ODPYTYWANIE KOMEND ← Supabase ───────────────────────────
static void checkCommands() {
  String url = String(CLOUD_CMD_URL) + "?device_id=" + CFG_CLOUD_DEVICE_ID;

  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + ANON_KEY);
  http.addHeader("apikey", ANON_KEY);
  http.setTimeout(4000);

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    if (body == "null" || body.length() < 5) { http.end(); return; }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err && doc.containsKey("cmd")) {
      const char* cmd   = doc["cmd"]   | "";
      float       value = doc["value"] | 0.0f;
      const char* id    = doc["id"]    | "";

      const char* strVal = "";
      if (!doc["extra"].isNull() && doc["extra"].is<JsonObject>()) {
        strVal = doc["extra"]["str"] | "";
      }

      executeCommand(cmd, value, strVal, id);
    }
  }
  http.end();
}

// ── GŁÓWNA PĘTLA ─────────────────────────────────────────────
void cloudReportLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();

  if (now - lastCloudReport >= CLOUD_REPORT_INTERVAL) {
    lastCloudReport = now;
    sendTelemetry();
  }

  if (now - lastCmdCheck >= CLOUD_CMD_INTERVAL) {
    lastCmdCheck = now;
    checkCommands();
  }
}