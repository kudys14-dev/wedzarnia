var currentPath='/';
var KNOWN_DIRS=['/profiles/','/backup/','/logs/','/web/'];

function navigateTo(p){currentPath=p;updateBreadcrumb(p);loadPath(p);var b=document.getElementById('btn-del-folder');if(b)b.hidden=(p==='/');var l=document.getElementById('current-path-label');if(l)l.textContent=p;}
function updateBreadcrumb(p){var bc=document.getElementById('breadcrumb');if(!bc)return;bc.innerHTML='<span onclick="navigateTo(\'/\')">/ root</span>';if(p==='/')return;var parts=p.split('/').filter(function(x){return x;});var built='/';parts.forEach(function(x){built+=x+'/';var sep=document.createElement('span');sep.textContent=' > ';var lnk=document.createElement('span');lnk.textContent=x;lnk.style.color='#90caf9';lnk.style.cursor='pointer';(function(c){lnk.onclick=function(){navigateTo(c);};})(built);bc.appendChild(sep);bc.appendChild(lnk);});}
function refreshCurrent(){navigateTo(currentPath);}

var _editPath='';

function closeViewer(){var v=document.getElementById('viewer-wrap');if(v)v.hidden=true;var vc=document.getElementById('viewer-content');if(vc)vc.style.display='none';_editPath='';}

function viewFile(path){
  fetch('/files/read?path='+encodeURIComponent(path)).then(function(r){return r.text();}).then(function(t){openViewer(path,t,false);}).catch(function(){alert('Blad odczytu');});
}

function editFile(path){
  fetch('/files/read?path='+encodeURIComponent(path)).then(function(r){return r.text();}).then(function(t){openViewer(path,t,true);}).catch(function(){alert('Blad odczytu');});
}

function openViewer(path,text,editable){
  _editPath=path;
  var vp=document.getElementById('viewer-path');
  var vc=document.getElementById('viewer-content');
  var vw=document.getElementById('viewer-wrap');
  var vb=document.getElementById('viewer-btns');
  if(!vp||!vc||!vw)return;
  vp.textContent=path+(editable?' [EDYCJA]':'');
  if(editable){
    vc.style.display='block';
    vc.innerHTML='<textarea id="editor-ta" style="width:100%;min-height:320px;background:rgba(0,0,0,.5);color:#eee;border:1px solid rgba(255,255,255,.15);border-radius:6px;padding:10px;font-family:\'Courier New\',monospace;font-size:.82em;resize:vertical;box-sizing:border-box"></textarea>';
    document.getElementById('editor-ta').value=text;
    if(vb)vb.innerHTML='<button onclick="saveFile()" style="padding:8px 18px;background:linear-gradient(135deg,#2e7d32,#1b5e20);color:#fff;border:none;border-radius:7px;font-weight:700;cursor:pointer;margin-right:6px">Zapisz</button><button onclick="closeViewer()" style="padding:8px 14px;background:rgba(255,255,255,.1);color:#eee;border:none;border-radius:7px;cursor:pointer">Anuluj</button><span id="save-msg" style="margin-left:10px;font-size:.85em"></span>';
  } else {
    vc.style.display='block';
    vc.innerHTML='<pre style="white-space:pre-wrap;word-break:break-all;font-family:\'Courier New\',monospace;font-size:.82em;color:#ccc;padding:10px;background:rgba(0,0,0,.3);border-radius:6px;max-height:400px;overflow-y:auto"></pre>';
    vc.querySelector('pre').textContent=text;
    if(vb)vb.innerHTML='<button onclick="editFile(\''+path+'\')" style="padding:8px 14px;background:linear-gradient(135deg,#1565c0,#0d47a1);color:#fff;border:none;border-radius:7px;cursor:pointer;margin-right:6px">Edytuj</button><button onclick="closeViewer()" style="padding:8px 14px;background:rgba(255,255,255,.1);color:#eee;border:none;border-radius:7px;cursor:pointer">Zamknij</button>';
  }
  vw.hidden=false;
  vw.scrollIntoView({behavior:'smooth'});
}

function saveFile(){
  var ta=document.getElementById('editor-ta');
  var msg=document.getElementById('save-msg');
  if(!ta||!_editPath)return;
  if(msg){msg.textContent='Zapisywanie...';msg.style.color='#aaa';}
  fetch('/files/upload',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:_editPath,data:ta.value})})
    .then(function(r){return r.json();})
    .then(function(d){
      if(msg){msg.textContent=d.ok?'Zapisano!':'Blad: '+(d.message||'?');msg.style.color=d.ok?'#4caf50':'#f44336';}
    })
    .catch(function(e){if(msg){msg.textContent='Blad: '+e;msg.style.color='#f44336';}});
}

function downloadFile(path){
  fetch('/files/read?path='+encodeURIComponent(path)).then(function(r){return r.text();}).then(function(t){var blob=new Blob([t],{type:'text/plain;charset=utf-8'});var url=URL.createObjectURL(blob);var a=document.createElement('a');a.href=url;a.download=path.split('/').pop();document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);}).catch(function(){alert('Blad pobierania');});
}

function deleteFile(path){
  if(!confirm('Usunac: '+path+'?'))return;
  fetch('/files/delete?path='+encodeURIComponent(path),{method:'POST'}).then(function(r){return r.json();}).then(function(d){if(d.ok)refreshCurrent();else alert('Blad: '+(d.message||'?'));}).catch(function(){alert('Blad polaczenia');});
}

function clearFolder(dir){
  if(!confirm('Usunac wszystkie pliki z '+dir+'?'))return;
  fetch('/files/list?dir='+encodeURIComponent(dir)).then(function(r){return r.json();}).then(function(data){var files=(data.files||[]).filter(function(f){return!f.name.endsWith('.dir');});if(!files.length){alert('Folder pusty.');return;}Promise.all(files.map(function(f){return fetch('/files/delete?path='+encodeURIComponent(f.name),{method:'POST'});})).then(function(){alert('Usunieto '+files.length+' plikow z '+dir);if(document.getElementById('file-list'))refreshCurrent();loadFlashInfo();});});
}

function deleteFolder(){if(currentPath!=='/')clearFolder(currentPath);}

function loadPath(path){
  var list=document.getElementById('file-list');
  if(!list)return;
  list.innerHTML='<li class=empty>Ladowanie...</li>';
  closeViewer();
  if(path==='/'){
    list.innerHTML='';
    KNOWN_DIRS.forEach(function(dir){var li=document.createElement('li');li.className='folder-item';li.innerHTML='<span class=fname>'+dir+'</span><div class=factions><button class=btn-view onclick="navigateTo(\''+dir+'\');event.stopPropagation()">Otworz</button><button class=btn-del onclick="clearFolder(\''+dir+'\');event.stopPropagation()">Wyczysc</button></div>';list.appendChild(li);});
    return;
  }
  fetch('/files/list?dir='+encodeURIComponent(path),{credentials:'include'}).then(function(r){return r.json();}).then(function(data){
    list.innerHTML='';
    var files=(data.files||[]).filter(function(f){return!f.name.endsWith('.dir');});
    if(!files.length){list.innerHTML='<li class=empty>Folder pusty</li>';return;}
    files.forEach(function(f){
      var name=f.name.split('/').pop();
      var li=document.createElement('li');li.className='folder-item';
      li.innerHTML='<span class=fname>'+name+'</span><span class=fsize>'+f.name+'</span>'
        +'<div class=factions>'
        +'<button class=btn-view onclick="viewFile(\''+f.name+'\');event.stopPropagation()">Podglad</button>'
        +'<button class="btn-view" style="background:#1565c0" onclick="editFile(\''+f.name+'\');event.stopPropagation()">Edytuj</button>'
        +'<button class=btn-dl onclick="downloadFile(\''+f.name+'\');event.stopPropagation()">Pobierz</button>'
        +'<button class=btn-del onclick="deleteFile(\''+f.name+'\');event.stopPropagation()">Usun</button>'
        +'</div>';
      list.appendChild(li);
    });
  }).catch(function(){list.innerHTML='<li class=empty style="color:#f44336">Blad</li>';});
}

function loadFlashInfo(){
  fetch('/flash/info',{credentials:'include'}).then(function(r){return r.json();}).then(function(d){
    var st=document.getElementById('flashStatus');if(st)st.textContent=d.ok?'OK (W25Q128)':'Blad';
    var jd=document.getElementById('flashJedec');if(jd)jd.textContent=d.jedec||'-';
    var sz=document.getElementById('flashSize');if(sz)sz.textContent=d.size||'-';
    var us=document.getElementById('flashUsage');var fb=document.getElementById('flashBar');
    if(d.ok&&us&&fb){var used=parseInt(d.used)||0;var free=parseInt(d.free)||0;var pct=(used+free)>0?Math.round(used/(used+free)*100):0;us.textContent=used+' zajetych / '+free+' wolnych ('+pct+'%)';fb.style.width=pct+'%';}
  }).catch(function(){var st=document.getElementById('flashStatus');if(st)st.textContent='Blad polaczenia';});
}

document.addEventListener('DOMContentLoaded',function(){loadFlashInfo();if(document.getElementById('file-list')){navigateTo('/');setInterval(loadFlashInfo,30000);}});
