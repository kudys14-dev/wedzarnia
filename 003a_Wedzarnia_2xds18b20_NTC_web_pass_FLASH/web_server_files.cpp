// web_server_files.cpp - Menadżer plików Flash dla interfejsu WWW
// [MOD] W25Q128 zamiast SD card - wszystkie operacje plikowe przez flash_storage API
#include "web_server_files.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "flash_storage.h"
#include <WebServer.h>

// ============================================================
// STRONA HTML MENEDŻERA PLIKÓW
// ============================================================
static const char HTML_FILE_MANAGER[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="pl"><head>
<meta charset="utf-8"><title>Menadżer plików</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="/style.css">
<style>
.file-list{list-style:none;padding:0;margin:10px 0}
.file-item{display:flex;justify-content:space-between;align-items:center;
  padding:10px 14px;background:rgba(0,0,0,.25);border-radius:8px;margin-bottom:6px;
  border:1px solid rgba(255,255,255,.08)}
.file-item .fname{font-family:'Courier New',monospace;font-size:.9em;color:#90caf9;word-break:break-all}
.file-item .fsize{color:#888;font-size:.8em;margin-left:10px;white-space:nowrap}
.file-actions{display:flex;gap:6px}
.file-actions button{padding:5px 10px;border:none;border-radius:6px;font-size:.8em;cursor:pointer;font-weight:600}
.btn-dl{background:#1976d2;color:#fff}
.btn-del{background:#c62828;color:#fff}
.storage-bar{height:10px;background:rgba(0,0,0,.3);border-radius:5px;overflow:hidden;margin:10px 0}
.storage-fill{height:100%;background:linear-gradient(90deg,#4caf50,#8bc34a);border-radius:5px;transition:width .3s}
#upload-progress{display:none;margin-top:10px}
</style>
</head><body>
<div class="page-wrap">
<div class="page-header"><h2>📁 Menadżer plików Flash</h2></div>

<div class="card">
<h3>💾 Pamięć Flash</h3>
<div class="row"><span class="lbl">Status</span><span class="val" id="flash-status">...</span></div>
<div class="row"><span class="lbl">Zajęte / Całkowite</span><span class="val" id="flash-usage">...</span></div>
<div class="storage-bar"><div class="storage-fill" id="flash-bar" style="width:0%"></div></div>
</div>

<div class="card">
<h3>📂 Profile (.prof)</h3>
<ul class="file-list" id="profiles-list"><li style="color:#888">Ładowanie...</li></ul>
</div>

<div class="card">
<h3>📦 Backupy (.bak)</h3>
<ul class="file-list" id="backups-list"><li style="color:#888">Ładowanie...</li></ul>
</div>

<div class="card">
<h3>📤 Wyślij plik na Flash</h3>
<label>Ścieżka docelowa (np. /profiles/nowy.prof)</label>
<input type="text" id="upload-path" placeholder="/profiles/nazwa.prof">
<label>Zawartość pliku</label>
<textarea id="upload-content" rows="6" style="width:100%;padding:11px 14px;background:rgba(0,0,0,.35);color:#eee;border:1px solid rgba(255,255,255,.15);border-radius:9px;font-family:'Courier New',monospace;font-size:.9em;resize:vertical"></textarea>
<button class="btn btn-primary" onclick="uploadFile()">📤 Wyślij</button>
<div id="upload-msg" style="margin-top:8px;font-size:.9em"></div>
</div>

<a href="/" class="back-link">← Powrót do panelu</a>
</div>
<script>
function loadFlashInfo(){
  fetch('/flash/info').then(r=>r.json()).then(d=>{
    document.getElementById('flash-status').textContent=d.ready?'OK':'Błąd';
    document.getElementById('flash-status').className='val '+(d.ready?'ok':'err');
    if(d.ready){
      var used=d.usedSectors||0,total=d.totalSectors||4096;
      var pct=Math.round(used/total*100);
      document.getElementById('flash-usage').textContent=used+' / '+total+' sektorów ('+pct+'%)';
      document.getElementById('flash-bar').style.width=pct+'%';
    }
  }).catch(()=>{
    document.getElementById('flash-status').textContent='Błąd połączenia';
    document.getElementById('flash-status').className='val err';
  });
}
function loadFileList(prefix,elementId){
  fetch('/files/list?dir='+encodeURIComponent(prefix)).then(r=>r.json()).then(data=>{
    var el=document.getElementById(elementId);
    if(!data.files||data.files.length===0){
      el.innerHTML='<li style="color:#666">Brak plików</li>';return;
    }
    el.innerHTML='';
    data.files.forEach(function(f){
      var li=document.createElement('li');li.className='file-item';
      li.innerHTML='<span class="fname">'+f.name+'</span>'+
        '<div class="file-actions">'+
        '<button class="btn-dl" onclick="viewFile(\''+f.name+'\')">👁</button>'+
        '<button class="btn-del" onclick="deleteFile(\''+f.name+'\')">🗑</button>'+
        '</div>';
      el.appendChild(li);
    });
  }).catch(()=>{
    document.getElementById(elementId).innerHTML='<li style="color:#f44336">Błąd ładowania</li>';
  });
}
function viewFile(path){
  fetch('/files/read?path='+encodeURIComponent(path)).then(r=>r.text()).then(t=>{
    alert('Zawartość '+path+':\n\n'+t);
  }).catch(()=>alert('Błąd odczytu pliku'));
}
function deleteFile(path){
  if(!confirm('Usunąć '+path+'?'))return;
  fetch('/files/delete?path='+encodeURIComponent(path),{method:'POST'}).then(r=>r.json()).then(d=>{
    alert(d.ok?'Usunięto!':'Błąd: '+(d.message||''));
    loadAll();
  }).catch(()=>alert('Błąd połączenia'));
}
function uploadFile(){
  var path=document.getElementById('upload-path').value.trim();
  var content=document.getElementById('upload-content').value;
  var msg=document.getElementById('upload-msg');
  if(!path){msg.textContent='Podaj ścieżkę!';msg.style.color='#f44336';return;}
  msg.textContent='Wysyłanie...';msg.style.color='#aaa';
  fetch('/files/write',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'path='+encodeURIComponent(path)+'&data='+encodeURIComponent(content)
  }).then(r=>r.json()).then(d=>{
    msg.textContent=d.ok?'Zapisano!':'Błąd: '+(d.message||'');
    msg.style.color=d.ok?'#4caf50':'#f44336';
    if(d.ok)loadAll();
  }).catch(()=>{msg.textContent='Błąd połączenia';msg.style.color='#f44336';});
}
function loadAll(){
  loadFlashInfo();
  loadFileList('/profiles/','profiles-list');
  loadFileList('/backup/','backups-list');
}
loadAll();
setInterval(loadFlashInfo,30000);
</script>
</body></html>)rawliteral";

// ============================================================
// API ENDPOINTS
// ============================================================

// Wymaga autoryzacji - helper (taki sam jak w web_server.cpp)
static bool requireAuthFiles() {
    if (!server.authenticate(storage_get_auth_user(), storage_get_auth_pass())) {
        server.requestAuthentication(BASIC_AUTH, "Wedzarnia", "Wymagane logowanie");
        return false;
    }
    return true;
}

// GET /files - strona menedżera plików
static void handleFileManagerPage() {
    if (!requireAuthFiles()) return;
    server.send_P(200, "text/html", HTML_FILE_MANAGER);
}

// GET /files/list?dir=/profiles/ - listowanie plików z danym prefiksem
static void handleFileList() {
    if (!requireAuthFiles()) return;
    String dir = server.hasArg("dir") ? server.arg("dir") : "/";
    
    char files[MAX_FLASH_FILES][MAX_FILENAME_LEN];
    int count = flash_list_files(dir.c_str(), files, MAX_FLASH_FILES);
    
    String json = "{\"files\":[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"name\":\"" + String(files[i]) + "\"}";
    }
    json += "]}";
    
    server.send(200, "application/json", json);
}

// GET /files/read?path=/profiles/test.prof - odczyt zawartości pliku
static void handleFileRead() {
    if (!requireAuthFiles()) return;
    
    if (!server.hasArg("path")) {
        server.send(400, "application/json", "{\"error\":\"Brak parametru path\"}");
        return;
    }
    
    String path = server.arg("path");
    
    if (!flash_file_exists(path.c_str())) {
        server.send(404, "application/json", "{\"error\":\"Plik nie istnieje\"}");
        return;
    }
    
    String content = flash_file_read_string(path.c_str());
    server.send(200, "text/plain", content);
}

// POST /files/write - zapis pliku (path + data w POST body)
static void handleFileWrite() {
    if (!requireAuthFiles()) return;
    
    if (!server.hasArg("path") || !server.hasArg("data")) {
        server.send(400, "application/json", "{\"ok\":false,\"message\":\"Brak path lub data\"}");
        return;
    }
    
    String path = server.arg("path");
    String data = server.arg("data");
    
    if (flash_file_write_string(path.c_str(), data)) {
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Zapisano\"}");
    } else {
        server.send(500, "application/json", "{\"ok\":false,\"message\":\"Błąd zapisu\"}");
    }
}

// POST /files/delete?path=/profiles/test.prof - usunięcie pliku
static void handleFileDelete() {
    if (!requireAuthFiles()) return;
    
    if (!server.hasArg("path")) {
        server.send(400, "application/json", "{\"ok\":false,\"message\":\"Brak parametru path\"}");
        return;
    }
    
    String path = server.arg("path");
    
    if (!flash_file_exists(path.c_str())) {
        server.send(404, "application/json", "{\"ok\":false,\"message\":\"Plik nie istnieje\"}");
        return;
    }
    
    if (flash_file_delete(path.c_str())) {
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Usunięto\"}");
    } else {
        server.send(500, "application/json", "{\"ok\":false,\"message\":\"Błąd usuwania\"}");
    }
}

// ============================================================
// REJESTRACJA ROUTE'ÓW
// ============================================================
void registerFileManagerRoutes() {
    server.on("/files",        HTTP_GET,  handleFileManagerPage);
    server.on("/files/list",   HTTP_GET,  handleFileList);
    server.on("/files/read",   HTTP_GET,  handleFileRead);
    server.on("/files/write",  HTTP_POST, handleFileWrite);
    server.on("/files/delete", HTTP_POST, handleFileDelete);
    
    LOG_FMT(LOG_LEVEL_INFO, "File manager routes registered");
}
