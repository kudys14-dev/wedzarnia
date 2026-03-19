// web_server.cpp
// [NEW v3] WebSocket /ws (port 81) zamiast HTTP polling /status
//          index.html serwowany z /web/index.html na W25Q128
//          HTML_TEMPLATE_MAIN usunięty z PROGMEM
#include "web_server.h"
#include "web_server_files.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "flash_storage.h"
#include "process.h"
#include "outputs.h"
#include "sensors.h"
#include <WiFi.h>
#include <Update.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <WebSocketsServer.h>
#include "notifications.h"

// =================================================================
// WebSocket - port 81
// =================================================================
static WebSocketsServer ws(81);

static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    (void)payload; (void)length;
    if (type == WStype_CONNECTED) {
        LOG_FMT(LOG_LEVEL_INFO, "WS[%u] connected from %s",
                num, ws.remoteIP(num).toString().c_str());
    } else if (type == WStype_DISCONNECTED) {
        LOG_FMT(LOG_LEVEL_INFO, "WS[%u] disconnected", num);
    }
}

// Buduje JSON statusu - współdzielony między WS a /status HTTP
static void buildStatusJson(char* buf, size_t bufSize) {
    double tc, tc1, tc2, tm, ts;
    int pm, fm, sm;
    ProcessState st;
    unsigned long elapsedSec = 0, stepTotalSec = 0, remainingSec = 0;
    const char* stepName = "";
    char activeProfile[64] = "Brak";

    state_lock();
    st   = g_currentState;
    tc   = g_tChamber;  tc1 = g_tChamber1;  tc2 = g_tChamber2;
    tm   = g_tMeat;     ts  = g_tSet;
    pm   = g_powerMode; fm  = g_fanMode;     sm  = g_manualSmokePwm;
    remainingSec = g_processStats.remainingProcessTimeSec;
    strncpy(activeProfile, storage_get_profile_path(), sizeof(activeProfile)-1);
    activeProfile[sizeof(activeProfile)-1] = '\0';
    if (st == ProcessState::RUNNING_MANUAL) {
        elapsedSec = (millis() - g_processStartTime) / 1000;
    } else if (st == ProcessState::RUNNING_AUTO) {
        elapsedSec = (millis() - g_stepStartTime) / 1000;
        if (g_currentStep < g_stepCount) {
            stepName     = g_profile[g_currentStep].name;
            stepTotalSec = g_profile[g_currentStep].minTimeMs / 1000;
        }
    }
    state_unlock();

    const char* modeStr;
    switch (st) {
        case ProcessState::IDLE:               modeStr = "IDLE";               break;
        case ProcessState::RUNNING_AUTO:       modeStr = "AUTO";               break;
        case ProcessState::RUNNING_MANUAL:     modeStr = "MANUAL";             break;
        case ProcessState::PAUSE_DOOR:         modeStr = "PAUZA: DRZWI";       break;
        case ProcessState::PAUSE_SENSOR:       modeStr = "PAUZA: CZUJNIK";     break;
        case ProcessState::PAUSE_OVERHEAT:     modeStr = "PAUZA: PRZEGRZANIE"; break;
        case ProcessState::PAUSE_HEATER_FAULT: modeStr = "AWARIA: GRZALKA";   break;
        case ProcessState::PAUSE_USER:         modeStr = "PAUZA UZYTK.";       break;
        case ProcessState::ERROR_PROFILE:      modeStr = "ERROR_PROFILE";      break;
        case ProcessState::SOFT_RESUME:        modeStr = "Wznawianie...";      break;
        default:                               modeStr = "UNKNOWN";            break;
    }
    const char* pmStr = pm==1?"1-grzalka":pm==2?"2-grzalki":pm==3?"3-grzalki":"Brak";
    const char* fmStr = fm==0?"OFF":fm==1?"ON":fm==2?"Cyklicznie":"Brak";

    char profClean[64];
    strncpy(profClean, activeProfile, sizeof(profClean));
    profClean[sizeof(profClean)-1] = '\0';
    if      (strstr(profClean,"/profiles/")) memmove(profClean, strstr(profClean,"/profiles/")+10, strlen(profClean));
    else if (strstr(profClean,"github:"))    memmove(profClean, profClean+7, strlen(profClean)-6);

    snprintf(buf, bufSize,
        "{\"tChamber\":%.1f,\"tChamber1\":%.1f,\"tChamber2\":%.1f,"
        "\"tMeat\":%.1f,\"tSet\":%.1f,"
        "\"powerMode\":%d,\"fanMode\":%d,\"smokePwm\":%d,"
        "\"mode\":\"%s\",\"state\":%d,"
        "\"powerModeText\":\"%s\",\"fanModeText\":\"%s\","
        "\"elapsedTimeSec\":%lu,\"stepName\":\"%s\","
        "\"stepTotalTimeSec\":%lu,\"activeProfile\":\"%s\","
        "\"remainingProcessTimeSec\":%lu}",
        tc,tc1,tc2,tm,ts,pm,fm,sm,
        modeStr,(int)st,pmStr,fmStr,
        elapsedSec,stepName,stepTotalSec,
        profClean,remainingSec);
}

// Broadcastuj status do wszystkich klientów WS - wywołuj co ~1s z taskWeb
void web_server_ws_broadcast() {
    if (ws.connectedClients() == 0) return;
    static char buf[700];
    buildStatusJson(buf, sizeof(buf));
    ws.broadcastTXT(buf);
}

// =================================================================
// AUTORYZACJA
// =================================================================
bool requireAuth() {
    if (!server.authenticate(storage_get_auth_user(), storage_get_auth_pass())) {
        server.requestAuthentication(BASIC_AUTH, "Wedzarnia", "Wymagane logowanie");
        return false;
    }
    return true;
}

// =================================================================
// POMOCNICZE
// =================================================================
static void serveWebFile(const char* flashPath, const char* mime,
                         const char* progmemFallback = nullptr) {
    if (flash_is_ready() && flash_file_exists(flashPath)) {
        String content = flash_file_read_string(flashPath);
        server.sendHeader("Cache-Control", "public, max-age=3600");
        server.send(200, mime, content);
    } else if (progmemFallback) {
        server.send_P(200, mime, progmemFallback);
    } else {
        server.send(503, "text/plain", String("Brak pliku: ") + flashPath);
    }
}

static void sendHtml(int code, const String& body) {
    server.send(code, "text/html; charset=utf-8", body);
}

// =================================================================
// STRONA BOOTSTRAPOWA - serwowana gdy /web/index.html nie istnieje
// Pozwala wgrać pliki web bez działającego interfejsu
// =================================================================
static const char BOOTSTRAP_HTML[] PROGMEM = R"RAW(<!DOCTYPE html>
<html lang="pl"><head><meta charset="utf-8"><title>Wędzarnia - Setup</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#1a1a2e,#16213e);color:#eee;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.box{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);border-radius:16px;padding:30px;max-width:480px;width:100%}
h1{font-size:1.6em;margin-bottom:6px}
.sub{color:#aaa;font-size:.9em;margin-bottom:24px}
.step{background:rgba(0,0,0,.3);border-radius:10px;padding:16px;margin-bottom:14px}
.step-title{font-weight:700;margin-bottom:10px;font-size:.95em}
.drop{border:2px dashed rgba(255,255,255,.2);border-radius:8px;padding:24px;text-align:center;cursor:pointer;transition:.2s;margin-bottom:10px}
.drop:hover,.drop.over{border-color:#2196f3;background:rgba(33,150,243,.08)}
.drop input{display:none}
.drop .icon{font-size:2em;margin-bottom:6px}
.drop .hint{font-size:.82em;color:#888;margin-top:4px}
.btn{width:100%;padding:13px;border:none;border-radius:9px;font-size:1em;font-weight:700;cursor:pointer;transition:.2s}
.btn-go{background:linear-gradient(135deg,#1976d2,#1565c0);color:#fff;margin-top:6px}
.btn-go:disabled{opacity:.4;cursor:not-allowed}
.btn-go:not(:disabled):hover{transform:translateY(-2px)}
#log{font-size:.82em;font-family:'Courier New',monospace;color:#aaa;margin-top:10px;min-height:18px}
.ok{color:#4caf50}.err{color:#f44336}.info{color:#64b5f6}
progress{width:100%;height:6px;border-radius:3px;margin-top:8px;display:none}
progress::-webkit-progress-bar{background:#333;border-radius:3px}
progress::-webkit-progress-value{background:#2196f3;border-radius:3px}
</style></head><body>
<div class="box">
<h1>🔥 Wędzarnia IoT</h1>
<div class="sub">Flash nie zawiera plików web. Wgraj je poniżej.</div>

<div class="step">
<div class="step-title">📁 Wgraj pliki <code>/web/</code></div>
<div class="drop" id="dropzone" onclick="document.getElementById('fileInput').click()"
     ondragover="event.preventDefault();this.classList.add('over')"
     ondragleave="this.classList.remove('over')"
     ondrop="handleDrop(event)">
  <div class="icon">📂</div>
  <div>Kliknij lub przeciągnij pliki</div>
  <div class="hint">index.html, style.css, flash.html, flash.js, ntfy.html…</div>
  <input type="file" id="fileInput" multiple onchange="handleFiles(this.files)">
</div>
<div id="queue" style="font-size:.82em;color:#888;margin-bottom:8px"></div>
<progress id="prog"></progress>
<button class="btn btn-go" id="btnUpload" disabled onclick="uploadAll()">⬆️ Wgraj wszystkie</button>
<div id="log"></div>
</div>

<div class="step">
<div class="step-title">ℹ️ Status flash</div>
<div id="flashInfo" style="font-size:.88em;color:#aaa">Ładowanie…</div>
</div>

<div class="step">
<div class="step-title">🗑️ Formatowanie flash</div>
<div style="font-size:.82em;color:#888;margin-bottom:10px">Usuwa wszystkie pliki i tworzy puste katalogi /web/ /profiles/ /backup/. Wymagane gdy flash jest uszkodzony lub po raz pierwszy.</div>
<button class="btn btn-go" id="btnFormat" onclick="formatFlash()" style="background:linear-gradient(135deg,#c62828,#b71c1c)">🗑️ Formatuj Flash</button>
<div id="fmtLog" style="font-size:.82em;margin-top:8px"></div>
</div>
</div>

<script>
var files = [];
function handleDrop(e){e.preventDefault();document.getElementById('dropzone').classList.remove('over');handleFiles(e.dataTransfer.files);}
function handleFiles(fl){
  for(var i=0;i<fl.length;i++) files.push(fl[i]);
  var q=document.getElementById('queue');
  q.textContent=files.length+' plik(ów): '+files.map(function(f){return f.name;}).join(', ');
  document.getElementById('btnUpload').disabled=files.length===0;
}
function log(msg,cls){var d=document.getElementById('log');d.innerHTML='<span class="'+(cls||'')+'">'+msg+'</span>';}
async function uploadAll(){
  var btn=document.getElementById('btnUpload');
  var prog=document.getElementById('prog');
  btn.disabled=true; prog.style.display='block';
  var ok=0,fail=0;
  for(var i=0;i<files.length;i++){
    var f=files[i];
    prog.value=(i/files.length)*100;
    log('Wgrywam: '+f.name+' ('+f.size+' B)…','info');
    try{
      var text=await f.text();
      // Metoda 1: raw body przez /files/upload_raw (omija problemy z parsowaniem)
      var r=await fetch('/files/upload_raw?path=/web/'+encodeURIComponent(f.name),{
        method:'POST',
        headers:{'Content-Type':'text/plain; charset=utf-8'},
        body:text
      });
      var t=await r.text();
      var errMsg=t;
      try{var j=JSON.parse(t);errMsg=j.message||t;}catch(e){}
      if(r.ok){ok++;log('✓ '+f.name+' ('+f.size+' B)','ok');}
      else{
        fail++;
        log('✗ '+f.name+' ['+r.status+']: '+errMsg,'err');
      }
    }catch(e){fail++;log('✗ '+f.name+': '+e,'err');}
    await new Promise(function(res){setTimeout(res,400);});
  }
  prog.value=100;
  log('Gotowe! OK: '+ok+' | Błąd: '+fail+(ok>0?' -- <a href="/" style="color:#64b5f6">Odśwież stronę</a>':''),'ok');
  btn.disabled=false; files=[];
}
function loadFlashInfo(){
  fetch('/flash/info').then(function(r){return r.json();}).then(function(d){
    document.getElementById('flashInfo').innerHTML=
      'Flash: '+(d.ok?'<span class="ok">OK</span>':'<span class="err">BLAD</span>')+
      ' | Wolne: '+d.free+' | Uzyte: '+d.used;
  }).catch(function(){document.getElementById('flashInfo').textContent='Brak danych';});
}
function formatFlash(){
  if(!confirm('Formatowac flash? Wszystkie pliki zostana usuniete!')) return;
  var btn=document.getElementById('btnFormat');
  var log=document.getElementById('fmtLog');
  btn.disabled=true;
  log.innerHTML='<span class="info">Formatowanie...</span>';
  fetch('/flash/format',{method:'POST'})
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.ok){
        log.innerHTML='<span class="ok">OK: '+d.message+'</span>';
        loadFlashInfo();
      } else {
        log.innerHTML='<span class="err">Blad: '+d.message+'</span>';
      }
      btn.disabled=false;
    })
    .catch(function(e){
      log.innerHTML='<span class="err">Blad polaczenia: '+e+'</span>';
      btn.disabled=false;
    });
}
loadFlashInfo();
</script></body></html>)RAW";

// =================================================================
// WSPÓLNY CSS - fallback gdy /web/style.css nie istnieje na flash
// =================================================================
static const char CSS_COMMON[] PROGMEM =
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#1a1a2e,#16213e);color:#eee;padding:20px;min-height:100vh}"
".page-wrap{max-width:520px;margin:0 auto}"
".page-header{background:linear-gradient(135deg,#d32f2f,#c62828);padding:18px 22px;border-radius:14px;margin-bottom:22px;box-shadow:0 8px 20px rgba(211,47,47,.3)}"
".page-header h2{font-size:1.4em;margin:0}"
".card{background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.1);border-radius:14px;padding:20px;margin-bottom:16px;box-shadow:0 8px 32px rgba(0,0,0,.3)}"
".card h3{font-size:.8em;text-transform:uppercase;letter-spacing:1.5px;color:#aaa;margin-bottom:14px;padding-bottom:8px;border-bottom:1px solid rgba(255,255,255,.1)}"
".row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid rgba(255,255,255,.06)}"
".row:last-child{border:none}"
".lbl{color:#888;font-size:.9em}.val{font-weight:600;font-family:'Courier New',monospace;font-size:.95em}"
".val.ok{color:#4caf50}.val.err{color:#f44336}.val.warn{color:#ff9800}.val.info{color:#00bcd4}"
"label{display:block;margin-top:14px;margin-bottom:5px;font-size:.85em;color:#aaa;text-transform:uppercase;letter-spacing:.8px}"
"input[type=text],input[type=password],input[type=number],input[type=file],select,textarea{"
"width:100%;padding:11px 14px;background:rgba(0,0,0,.35);color:#eee;"
"border:1px solid rgba(255,255,255,.15);border-radius:9px;font-size:1em}"
"input:focus,select:focus,textarea:focus{outline:none;border-color:#2196f3;box-shadow:0 0 0 3px rgba(33,150,243,.2)}"
".btn{display:block;width:100%;padding:13px;margin-top:10px;border:none;border-radius:9px;"
"font-size:1em;font-weight:700;cursor:pointer;transition:all .25s;box-shadow:0 4px 14px rgba(0,0,0,.3)}"
".btn:hover{transform:translateY(-2px)}"
".btn-primary{background:linear-gradient(135deg,#1976d2,#1565c0);color:#fff}"
".btn-danger{background:linear-gradient(135deg,#c62828,#b71c1c);color:#fff}"
".btn:disabled{opacity:.4;cursor:not-allowed;transform:none!important}"
".warn-box{background:rgba(211,47,47,.12);border:1px solid rgba(211,47,47,.45);border-radius:12px;padding:16px;margin-bottom:16px}"
".warn-box h3{color:#ef9a9a}.warn-box p{margin-top:8px;font-size:.9em;color:#ccc;line-height:1.5}"
".check-label{display:flex;align-items:center;gap:10px;cursor:pointer;font-size:.95em;"
"padding:12px;background:rgba(0,0,0,.2);border-radius:8px;margin:14px 0}"
".note{font-size:.82em;color:#888;margin-top:14px;line-height:1.6;padding:12px;background:rgba(0,0,0,.2);border-radius:8px}"
".btn-row{display:flex;gap:8px;margin-top:14px}"
".btn-row button{flex:1;padding:12px;border:none;border-radius:9px;font-size:.95em;font-weight:600;cursor:pointer}"
".btn-add{background:linear-gradient(135deg,#2196f3,#1565c0);color:#fff}"
".btn-save{background:linear-gradient(135deg,#4caf50,#388e3c);color:#fff}"
".btn-pc{background:linear-gradient(135deg,#607d8b,#455a64);color:#fff}"
".btn-clear{background:linear-gradient(135deg,#c62828,#b71c1c);color:#fff}"
".step-preview{padding:8px 12px;background:rgba(0,0,0,.25);border-radius:6px;margin-bottom:5px;cursor:pointer;font-size:.9em;font-family:'Courier New',monospace}"
".step-preview:hover{background:rgba(33,150,243,.15)}"
".back-link{display:inline-block;margin-top:18px;color:#64b5f6;text-decoration:none;font-size:.95em}"
".back-link:hover{color:#90caf9}"
".check-row{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:.9em}";

static void handleCommonCss() {
    server.sendHeader("Cache-Control", "public, max-age=86400");
    serveWebFile("/web/style.css", "text/css", CSS_COMMON);
}

// =================================================================
// STATUS JSON - /status HTTP (kompatybilność wsteczna)
// =================================================================
static const char* getStatusJSON() {
    static char buf[700];
    buildStatusJson(buf, sizeof(buf));
    return buf;
}

// =================================================================
// SYSINFO JSON
// =================================================================
static void handleSysInfoJson() {
    if (!requireAuth()) return;
    bool flashOk = flash_is_ready();
    char jedecStr[16];
    snprintf(jedecStr, sizeof(jedecStr), "0x%04X", flashOk ? flash_get_jedec_id() : 0);
    bool wifiConn = (WiFi.status() == WL_CONNECTED);
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rrStr;
    switch (rr) {
        case ESP_RST_POWERON:  rrStr="Power-on";    break;
        case ESP_RST_SW:       rrStr="Reset SW";    break;
        case ESP_RST_PANIC:    rrStr="Panic/Crash"; break;
        case ESP_RST_TASK_WDT: rrStr="WDT Task";   break;
        case ESP_RST_BROWNOUT: rrStr="Brownout";    break;
        default:               rrStr="Inny";        break;
    }
    static char json[900];
    snprintf(json, sizeof(json),
        "{\"heap_free\":%u,\"heap_total\":%u,\"heap_min\":%u,\"psram_total\":%u,"
        "\"uptime_sec\":%lu,\"cpu_freq\":%u,\"cpu_temp\":%.1f,\"reset_reason\":\"%s\","
        "\"flash_ok\":%s,\"flash_jedec\":\"%s\","
        "\"flash_used_sectors\":%u,\"flash_free_sectors\":%u,"
        "\"sensor_count\":%d,\"sensors_identified\":%s,"
        "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\","
        "\"wifi_ip\":\"%s\",\"ap_ip\":\"%s\",\"wifi_rssi\":%d,"
        "\"fw_name\":\"" FW_NAME "\",\"fw_version\":\"" FW_VERSION "\","
        "\"fw_author\":\"" FW_AUTHOR "\","
        "\"chip_model\":\"%s\",\"mac_addr\":\"%s\",\"flash_size\":%u}",
        ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getPsramSize(),
        millis()/1000, ESP.getCpuFreqMHz(), temperatureRead(), rrStr,
        flashOk?"true":"false", jedecStr,
        flashOk?flash_get_used_sectors():0u, flashOk?flash_get_free_sectors():0u,
        sensors.getDeviceCount(), areSensorsIdentified()?"true":"false",
        wifiConn?"true":"false",
        wifiConn?WiFi.SSID().c_str():"",
        wifiConn?WiFi.localIP().toString().c_str():"",
        WiFi.softAPIP().toString().c_str(),
        wifiConn?WiFi.RSSI():0,
        ESP.getChipModel(), WiFi.macAddress().c_str(), ESP.getFlashChipSize());
    server.send(200, "application/json", json);
}

// =================================================================
// CZUJNIKI API
// =================================================================
static void handleSensorInfo() {
    if (!requireAuth()) return;
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"total_sensors\":%d,\"chamber1_index\":%d,\"chamber2_index\":%d,"
        "\"identified\":%s,\"ntc_pin\":%d}",
        getTotalSensorCount(), getChamberSensor1Index(), getChamberSensor2Index(),
        areSensorsIdentified()?"true":"false", PIN_NTC);
    server.send(200, "application/json", buf);
}
static void handleSensorReassign() {
    if (!requireAuth()) return;
    if (server.hasArg("chamber1") && server.hasArg("chamber2")) {
        int c1 = server.arg("chamber1").toInt(), c2 = server.arg("chamber2").toInt();
        if (c1 >= 0 && c2 >= 0 && c1 != c2) {
            chamberSensor1Index = c1; chamberSensor2Index = c2; sensorsIdentified = true;
            server.send(200,"application/json","{\"status\":\"ok\"}");
        } else server.send(400,"application/json","{\"error\":\"Invalid indices\"}");
    } else server.send(400,"application/json","{\"error\":\"Missing parameters\"}");
}
static void handleSensorAutoDetect() {
    if (!requireAuth()) return;
    identifyAndAssignSensors();
    server.send(areSensorsIdentified()?200:500,"application/json",
        areSensorsIdentified()?"{\"message\":\"OK\"}":"{\"error\":\"Failed\"}");
}

// =================================================================
// FLASH API
// =================================================================
static void handleFlashInfo() {
    if (!requireAuth()) return;
    char json[256];
    bool ok = flash_is_ready();
    bool isIdle = false;
    if (state_lock()) { isIdle=(g_currentState==ProcessState::IDLE); state_unlock(); }
    if (!ok) {
        snprintf(json,sizeof(json),
            "{\"ok\":false,\"idle\":%s,\"jedec\":\"-\",\"size\":\"-\",\"used\":\"-\",\"free\":\"-\"}",
            isIdle?"true":"false");
    } else {
        char jedec[16]; snprintf(jedec,sizeof(jedec),"0x%04X",flash_get_jedec_id());
        snprintf(json,sizeof(json),
            "{\"ok\":true,\"idle\":%s,\"jedec\":\"%s\",\"size\":\"%lu MB\","
            "\"used\":\"%lu sektorów\",\"free\":\"%lu sektorów\"}",
            isIdle?"true":"false",jedec,flash_get_total_size()/(1024*1024),
            flash_get_used_sectors(),flash_get_free_sectors());
    }
    server.send(200,"application/json",json);
}
static void handleFlashFormat() {
    if (!requireAuth()) return;
    bool isIdle=false;
    if (state_lock()){isIdle=(g_currentState==ProcessState::IDLE);state_unlock();}
    if (!isIdle){server.send(200,"application/json","{\"ok\":false,\"message\":\"Zatrzymaj proces!\"}");return;}
    if (!flash_is_ready()){server.send(200,"application/json","{\"ok\":false,\"message\":\"Flash niedostępny!\"}");return;}
    if (flash_format()){
        flash_mkdir("/profiles"); flash_mkdir("/backup"); flash_mkdir("/web");
        server.send(200,"application/json","{\"ok\":true,\"message\":\"Sformatowano. Wgraj pliki /web/.\"}");
    } else {
        server.send(200,"application/json","{\"ok\":false,\"message\":\"Formatowanie nieudane.\"}");
    }
}

// =================================================================
// INICJALIZACJA SERWERA
// =================================================================
void web_server_init() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(CFG_AP_SSID, CFG_AP_PASS);
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    const char* ssid = storage_get_wifi_ssid();
    if (strlen(ssid) > 0) {
        WiFi.begin(ssid, storage_get_wifi_pass());
        Serial.printf("Connecting STA to %s...\n", ssid);
    }

    if (flash_is_ready() && !flash_dir_exists("/web")) flash_mkdir("/web");

    // -- WebSocket port 81 --------------------------------------
    ws.begin();
    ws.onEvent(onWsEvent);
    Serial.println("WebSocket started on port 81");

    // -- CSS ----------------------------------------------------
    server.on("/style.css", HTTP_GET, handleCommonCss);

    // -- Strona główna z /web/index.html na flash ---------------
    server.on("/", HTTP_GET, []() {
        serveWebFile("/web/index.html", "text/html; charset=utf-8",
                     BOOTSTRAP_HTML);   // fallback gdy brak pliku na flash
    });

    // -- Status HTTP (kompatybilność wsteczna) ------------------
    server.on("/status", HTTP_GET, []() {
        server.send(200, "application/json", getStatusJSON());
    });

    // -- API publiczne ------------------------------------------
    server.on("/api/profiles",        HTTP_GET,  [](){ server.send(200,"application/json",storage_list_profiles_json()); });
    server.on("/api/github_profiles", HTTP_GET,  [](){ server.send(200,"application/json",storage_list_github_profiles_json()); });
    server.on("/api/sysinfo",         HTTP_GET,  handleSysInfoJson);
    server.on("/api/sensors",         HTTP_GET,  handleSensorInfo);
    server.on("/api/sensors/reassign",   HTTP_POST, handleSensorReassign);
    server.on("/api/sensors/autodetect", HTTP_POST, handleSensorAutoDetect);

    // -- Flash API ----------------------------------------------
    server.on("/flash/info",   HTTP_GET,  handleFlashInfo);
    server.on("/flash/format", HTTP_POST, handleFlashFormat);
    server.on("/sd/info",      HTTP_GET,  handleFlashInfo);
    server.on("/sd/format",    HTTP_POST, handleFlashFormat);

    // -- Strony z /web/ na flash --------------------------------
    struct { const char* url; const char* file; bool noAuth; } pages[] = {
        { "/flash",    "/web/flash.html",    false },
        { "/flash2",   "/web/flash2.html",   false },
        { "/creator",  "/web/creator.html",  false },
        { "/sensors",  "/web/sensors.html",  false },
        { "/sysinfo",  "/web/sysinfo.html",  false },
        { "/update",   "/web/update.html",   false },
        { "/auth/set", "/web/auth_set.html", false },
        { "/wifi",     "/web/wifi.html",     true  },  // bez auth - konfiguracja sieci
        { "/upload",   "/web/upload.html",   false },
        { "/sd",       "/web/flash.html",    false },
    };
    for (auto& p : pages) {
        const char* url  = p.url;
        const char* file = p.file;
        bool noAuth = p.noAuth;
        server.on(url, HTTP_GET, [url, file, noAuth]() {
            if (!noAuth && !requireAuth()) return;
            serveWebFile(file, "text/html; charset=utf-8");
        });
    }

    // -- Pliki statyczne /web/ (JS, CSS specyficzne) ------------
    server.on("/web/flash.css", HTTP_GET, [](){
        server.sendHeader("Cache-Control","public, max-age=86400");
        serveWebFile("/web/flash.css","text/css");
    });
    server.on("/web/flash.js", HTTP_GET, [](){
        server.sendHeader("Cache-Control","public, max-age=86400");
        serveWebFile("/web/flash.js","application/javascript");
    });

    // -- Autoryzacja --------------------------------------------
    server.on("/auth/login", HTTP_GET, [](){
        if (!requireAuth()) return;
        server.sendHeader("Location","/"); server.send(302);
    });
    server.on("/auth/save", HTTP_POST, [](){
        if (!requireAuth()) return;
        String u=server.arg("user"),p=server.arg("pass"),p2=server.arg("pass2");
        if (u.isEmpty()||p.isEmpty()||p2.isEmpty()||u.length()>31||p.length()<4||p.length()>63||p!=p2) {
            sendHtml(400,"<html><body style='background:#111;color:#eee;padding:20px'>"
                "Nieprawid&#322;owe dane. <a href='/auth/set'>Wr&oacute;&cacute;</a></body></html>");
            return;
        }
        storage_save_auth_nvs(u.c_str(), p.c_str());
        server.requestAuthentication(BASIC_AUTH,"Wedzarnia","Haslo zmienione!");
    });

    // -- Profile ------------------------------------------------
    server.on("/profile/get", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (!server.hasArg("name")||!server.hasArg("source")){server.send(400,"text/plain","Brak parametrów");return;}
        server.send(200,"application/json",storage_get_profile_as_json(server.arg("name").c_str()));
    });
    server.on("/profile/select", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (!server.hasArg("name")||!server.hasArg("source")){server.send(400,"text/plain","Brak parametrów");return;}
        String name=server.arg("name"),src=server.arg("source");
        bool ok=false;
        if (src=="sd"){ storage_save_profile_path_nvs(("/profiles/"+name).c_str()); ok=storage_load_profile(); }
        else if (src=="github"){ storage_save_profile_path_nvs(("github:"+name).c_str()); ok=storage_load_github_profile(name.c_str()); }
        server.send(ok?200:500,"text/plain",ok?"OK, profil "+name+" załadowany.":"Błąd ładowania.");
    });
    server.on("/profile/create", HTTP_POST, [](){
        if (!requireAuth()) return;
        if (!server.hasArg("filename")||!server.hasArg("data")){server.send(400,"text/plain","Brak danych.");return;}
        String fname=server.arg("filename"),data=server.arg("data");
        if (fname.isEmpty()||data.isEmpty()){server.send(400,"text/plain","Puste pola.");return;}
        if (!fname.endsWith(".prof")) fname+=".prof";
        if (data.length()>32768){server.send(413,"text/plain","Za duży.");return;}
        esp_task_wdt_reset();
        String path="/profiles/"+fname;
        if (flash_file_write_string(path.c_str(),data))
            server.send(200,"text/plain","Profil '"+fname+"' zapisany ("+String(data.length())+" B)");
        else
            server.send(500,"text/plain","Błąd zapisu! Wolne: "+String(flash_get_free_sectors())+" sekt.");
        esp_task_wdt_reset();
    });
    server.on("/profile/reload", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (storage_reinit_flash()){storage_load_profile();server.send(200,"text/plain","Flash odświeżony.");}
        else server.send(500,"text/plain","Błąd reinicjalizacji!");
    });

    // -- Sterowanie ---------------------------------------------
    server.on("/auto/next_step", HTTP_GET, [](){
        if (!requireAuth()) return;
        state_lock();
        if (g_currentState==ProcessState::RUNNING_AUTO&&g_currentStep<g_stepCount)
            g_profile[g_currentStep].minTimeMs=0;
        state_unlock();
        server.send(200,"text/plain","OK");
    });
    server.on("/timer/reset", HTTP_GET, [](){
        if (!requireAuth()) return;
        state_lock();
        if      (g_currentState==ProcessState::RUNNING_MANUAL) g_processStartTime=millis();
        else if (g_currentState==ProcessState::RUNNING_AUTO)   g_stepStartTime=millis();
        state_unlock();
        server.send(200,"text/plain","OK");
    });
    server.on("/mode/manual", HTTP_GET, [](){
        if (!requireAuth()) return; process_start_manual(); server.send(200,"text/plain","OK");
    });
    server.on("/auto/start", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (storage_load_profile()){process_start_auto();server.send(200,"text/plain","OK");}
        else server.send(500,"text/plain","Profile error");
    });
    server.on("/auto/stop", HTTP_GET, [](){
        if (!requireAuth()) return;
        allOutputsOff();
        state_lock(); g_currentState=ProcessState::IDLE; state_unlock();
        server.send(200,"text/plain","OK");
    });

    // -- Ustawienia manualne ------------------------------------
    server.on("/manual/set", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (server.hasArg("tSet")){
            double val=constrain(server.arg("tSet").toFloat(),CFG_T_MIN_SET,CFG_T_MAX_SET);
            state_lock();g_tSet=val;state_unlock(); storage_save_manual_settings_nvs();
        }
        server.send(200,"text/plain","OK");
    });
    server.on("/manual/power", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (server.hasArg("val")){
            int val=constrain(server.arg("val").toInt(),CFG_POWERMODE_MIN,CFG_POWERMODE_MAX);
            state_lock();g_powerMode=val;state_unlock(); storage_save_manual_settings_nvs();
        }
        server.send(200,"text/plain","OK");
    });
    server.on("/manual/smoke", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (server.hasArg("val")){
            int val=constrain(server.arg("val").toInt(),CFG_SMOKE_PWM_MIN,CFG_SMOKE_PWM_MAX);
            state_lock();g_manualSmokePwm=val;state_unlock(); storage_save_manual_settings_nvs();
        }
        server.send(200,"text/plain","OK");
    });
    server.on("/manual/fan", HTTP_GET, [](){
        if (!requireAuth()) return;
        if (server.hasArg("mode")){state_lock();g_fanMode=constrain(server.arg("mode").toInt(),0,2);state_unlock();}
        if (server.hasArg("on"))  {state_lock();g_fanOnTime =max(1000UL,(unsigned long)server.arg("on").toInt()*1000UL); state_unlock();}
        if (server.hasArg("off")) {state_lock();g_fanOffTime=max(1000UL,(unsigned long)server.arg("off").toInt()*1000UL);state_unlock();}
        storage_save_manual_settings_nvs();
        server.send(200,"text/plain","OK");
    });

    // -- WiFi save ---------------------------------------------
    server.on("/wifi/save", HTTP_POST, [](){
        if (!requireAuth()) return;
        if (server.hasArg("ssid")&&server.hasArg("pass")){
            storage_save_wifi_nvs(server.arg("ssid").c_str(),server.arg("pass").c_str());
            WiFi.begin(storage_get_wifi_ssid(),storage_get_wifi_pass());
        }
        sendHtml(200,"<html><head><meta charset='utf-8'></head>"
            "<body style='background:#1a1a2e;color:#eee;padding:20px;font-family:sans-serif'>"
            "&#x23F3; &#x141;&#x105;czenie... <a href='/' style='color:#64b5f6'>Wr&oacute;&cacute;</a>"
            "</body></html>");
    });

    // -- OTA UPDATE ---------------------------------------------
    server.on("/update", HTTP_POST,
        [](){
            if (!requireAuth()) return;
            server.sendHeader("Connection","close");
            bool ok=!Update.hasError();
            server.send(200,"text/plain",ok?"OK":Update.errorString());
            if (ok){delay(500);ESP.restart();}
        },
        [](){
            if (!server.authenticate(storage_get_auth_user(),storage_get_auth_pass())){
                server.requestAuthentication(BASIC_AUTH,"Wedzarnia","Wymagane logowanie"); return;
            }
            HTTPUpload& upload=server.upload();
            if (upload.status==UPLOAD_FILE_START){
                Serial.printf("[OTA] Start: %s\n",upload.filename.c_str());
                esp_task_wdt_config_t cfg={.timeout_ms=60000,.idle_core_mask=0,.trigger_panic=false};
                esp_task_wdt_reconfigure(&cfg); esp_task_wdt_reset();
                Update.begin(UPDATE_SIZE_UNKNOWN);
            } else if (upload.status==UPLOAD_FILE_WRITE){
                esp_task_wdt_reset(); Update.write(upload.buf,upload.currentSize);
            } else if (upload.status==UPLOAD_FILE_END){
                esp_task_wdt_reset(); Update.end(true);
                Serial.printf("[OTA] Done: %u B\n",upload.totalSize);
            } else if (upload.status==UPLOAD_FILE_ABORTED){
                Update.abort();
            }
            yield();
        }
    );


    // -- Ntfy powiadomienia -------------------------------------
    server.on("/ntfy", HTTP_GET, [](){
        serveWebFile("/web/ntfy.html", "text/html; charset=utf-8");
    });
    server.on("/ntfy/config", HTTP_GET, [](){
        if (!requireAuth()) return;
        char json[200];
        snprintf(json, sizeof(json),
            "{\"enabled\":%s,\"topic\":\"%s\",\"server\":\"%s\"}",
            storage_get_ntfy_enabled()?"true":"false",
            storage_get_ntfy_topic(),
            strlen(storage_get_ntfy_server())>0
                ? storage_get_ntfy_server() : "https://ntfy.sh");
        server.send(200, "application/json", json);
    });
    server.on("/ntfy/save", HTTP_POST, [](){
        if (!requireAuth()) return;
        bool en = server.arg("enabled") == "1";
        String topic = server.arg("topic");
        String srv   = server.arg("server");
        if (topic.isEmpty()) { server.send(400,"text/plain","Brak topicu"); return; }
        if (srv.isEmpty()) srv = "https://ntfy.sh";
        storage_save_ntfy_enabled(en);
        storage_save_ntfy_topic(topic.c_str());
        storage_save_ntfy_server(srv.c_str());
        notifications_init();
        server.send(200, "text/plain", "Zapisano \u2713");
    });
    server.on("/ntfy/test", HTTP_POST, [](){
        if (!requireAuth()) return;
        if (WiFi.status() != WL_CONNECTED) {
            server.send(200, "text/plain", "Brak WiFi \u2014 pol\u0105cz z sieci\u0105 domow\u0105");
            return;
        }
        notify_send("\U0001F514 Test w\u0119dzarni",
                    "Powiadomienia dzia\u0142aj\u0105 prawid\u0142owo!", 3, "white_check_mark");
        server.send(200, "text/plain", "\u2713 Wys\u0142ano \u2014 sprawd\u017a apk\u0119 ntfy");
    });

    // [NEW] /files/upload_raw - raw body, path w query stringu
    server.on("/files/upload_raw", HTTP_POST, [](){
        if (!requireAuth()) return;
        if (!server.hasArg("path")) {
            server.send(400, "application/json", "{\"ok\":false,\"message\":\"Brak path\"}");
            return;
        }
        String path = server.arg("path");
        if (!path.startsWith("/")) path = "/" + path;
        String body = server.arg("plain");
        if (body.length() == 0) {
            server.send(400, "application/json", "{\"ok\":false,\"message\":\"Puste body\"}");
            return;
        }
        if (body.length() > 32768) {
            server.send(413, "application/json", "{\"ok\":false,\"message\":\"Za duzy\"}");
            return;
        }
        if (!flash_is_ready()) {
            server.send(500, "application/json", "{\"ok\":false,\"message\":\"Flash nie gotowy\"}");
            return;
        }
        esp_task_wdt_reset();
        if (flash_file_write_string(path.c_str(), body)) {
            server.send(200, "application/json", "{\"ok\":true}");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Zapis nieudany: %s (%dB) wolne:%lu sekt",
                path.c_str(), body.length(), flash_get_free_sectors());
            String resp = String("{\"ok\":false,\"message\":\"") + msg + "\"}";
            server.send(500, "application/json", resp);
        }
        esp_task_wdt_reset();
    });

    // [NEW] /files/upload - JSON body {path, data}, uzywany przez webupload.html
    server.on("/files/upload", HTTP_POST, [](){
        if (!requireAuth()) return;
        String body = server.arg("plain");
        if (body.isEmpty()) {
            server.send(400, "application/json", "{\"ok\":false,\"message\":\"Puste body\"}");
            return;
        }
        // Prosty parser JSON - wyciaga wartosci string dla podanego klucza
        auto jsonGetStr = [](const String& src, const char* key) -> String {
            String needle = String("\"") + key + "\":\"";
            int s = src.indexOf(needle);
            if (s < 0) return String();
            s += needle.length();
            String result;
            bool esc = false;
            for (int i = s; i < (int)src.length(); i++) {
                char c = src[i];
                if (esc) {
                    if      (c == 'n')  result += '\n';
                    else if (c == 'r')  result += '\r';
                    else if (c == 't')  result += '\t';
                    else if (c == '\\') result += '\\';
                    else if (c == '"')  result += '"';
                    else                result += c;
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    break;
                } else {
                    result += c;
                }
            }
            return result;
        };
        String path = jsonGetStr(body, "path");
        String data = jsonGetStr(body, "data");
        if (path.isEmpty()) {
            server.send(400, "application/json", "{\"ok\":false,\"message\":\"Brak path w JSON\"}");
            return;
        }
        if (!path.startsWith("/")) path = "/" + path;
        if (data.length() > 32768) {
            server.send(413, "application/json", "{\"ok\":false,\"message\":\"Za duzy (max 32KB)\"}");
            return;
        }
        if (!flash_is_ready()) {
            server.send(500, "application/json", "{\"ok\":false,\"message\":\"Flash nie gotowy\"}");
            return;
        }
        String dir = path.substring(0, path.lastIndexOf('/'));
        if (dir.length() > 0 && !flash_dir_exists(dir.c_str()))
            flash_mkdir(dir.c_str());
        esp_task_wdt_reset();
        if (flash_file_write_string(path.c_str(), data)) {
            server.send(200, "application/json", "{\"ok\":true}");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Zapis nieudany: %s (%dB) wolne:%lu sekt",
                path.c_str(), data.length(), flash_get_free_sectors());
            String resp = String("{\"ok\":false,\"message\":\"") + msg + "\"}";
            server.send(500, "application/json", resp);
        }
        esp_task_wdt_reset();
    });

    
    // -- Menedzer plikow ------------------------------------------
    registerFileManagerRoutes();

    server.begin();
    Serial.println("HTTP server started on port 80");
}

void web_server_handle_client() {
    server.handleClient();
    ws.loop();
}