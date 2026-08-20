// wifiprov_page.h — the setup portal, embedded in flash (PROGMEM).
//
// WHY embedded and not served from LittleFS like data/index.html: the portal is the recovery path.
// It has to work when LittleFS is empty (board never had `pio run -t uploadfs`), half-written (a
// /fsup upload that dropped mid-transfer), or corrupt. If the page that fixes the network lived on
// the filesystem that the network is used to update, a bad upload would brick the board for a
// customer who has no USB cable. ~7 kB of flash out of 6.5 MB is a cheap insurance policy.
//
// Fully self-contained: no CDN, no web font, no external image — there is no internet in AP mode.
// Palette matches data/index.html. UI text is French like the rest of the web UI.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#pragma once
#include <Arduino.h>

static const char WIFIPROV_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>GottFA80 - Configuration WiFi</title><style>
:root{--bg:#0d1017;--panel:#161b24;--panel2:#1d2430;--line:#2a3340;--txt:#e7ecf3;--muted:#8b97a8;
--accent:#39b6ff;--ok:#3ddc84;--warn:#ffb020;--bad:#ff5d5d;--r:12px}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--txt);
font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;-webkit-text-size-adjust:100%}
.wrap{max-width:520px;margin:0 auto;padding:1rem .9rem 3rem}
h1{font-size:1.1rem;margin:.2rem 0 .1rem}
.sub{font-size:.78rem;color:var(--muted);margin-bottom:.9rem}
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);
padding:.8rem;margin-bottom:.8rem}
.card h2{font-size:.9rem;margin:0 0 .55rem}
.row{display:flex;align-items:center;gap:.5rem}
.grow{flex:1;min-width:0}
label{display:block;font-size:.75rem;color:var(--muted);margin:.5rem 0 .2rem}
input{width:100%;background:var(--panel2);border:1px solid var(--line);border-radius:8px;
color:var(--txt);padding:.55rem .6rem;font-size:.95rem;font-family:inherit}
input:focus{outline:none;border-color:var(--accent)}
button{background:var(--accent);color:#06121c;border:0;border-radius:8px;padding:.6rem .9rem;
font-size:.9rem;font-weight:650;font-family:inherit;cursor:pointer}
button.ghost{background:var(--panel2);color:var(--txt);border:1px solid var(--line);font-weight:500}
button.warn{background:var(--warn);color:#241700}
button.bad{background:var(--bad);color:#2a0000}
button[disabled]{opacity:.45;cursor:default}
.mt{margin-top:.6rem}
.net{display:flex;align-items:center;gap:.55rem;width:100%;text-align:left;background:var(--panel2);
border:1px solid var(--line);border-radius:8px;padding:.5rem .6rem;margin-bottom:.35rem;
color:var(--txt);font-size:.9rem;font-family:inherit;cursor:pointer}
.net:hover{border-color:var(--accent)}
.net .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net .ic{font-size:.8rem;color:var(--muted);flex:none}
.bars{flex:none;display:flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--ok)}
.bars i:nth-child(1){height:4px}.bars i:nth-child(2){height:7px}
.bars i:nth-child(3){height:10px}.bars i:nth-child(4){height:14px}
.msg{font-size:.82rem;border-radius:8px;padding:.55rem .65rem;margin-top:.6rem;display:none}
.msg.on{display:block}
.msg.i{background:#173047;color:#bfe4ff}.msg.o{background:#123526;color:#b7f2d4}
.msg.e{background:#3a1a1c;color:#ffc9c9}.msg.w{background:#3a2c10;color:#ffe1ac}
.kv{font-size:.8rem;color:var(--muted);line-height:1.55}
.kv b{color:var(--txt);font-weight:600}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.hint{font-size:.72rem;color:var(--muted);line-height:1.5;margin-top:.5rem}
details summary{font-size:.8rem;color:var(--muted);cursor:pointer;padding:.2rem 0}
.spin{display:inline-block;width:12px;height:12px;border:2px solid var(--line);
border-top-color:var(--accent);border-radius:50%;animation:sp .8s linear infinite;vertical-align:-2px}
@keyframes sp{to{transform:rotate(360deg)}}
</style></head><body><div class="wrap">
<h1>GottFA80 &middot; Configuration WiFi</h1>
<div class="sub">Flipper Gottlieb System 80 &mdash; carte PLuS</div>

<div class="card"><div class="kv" id="st">Chargement&hellip;</div></div>

<div class="card">
  <h2>1. Connecter a votre WiFi (box)</h2>
  <div class="row"><span class="grow kv">Reseaux detectes</span>
    <button class="ghost" id="rescan">Rechercher</button></div>
  <div id="nets" class="mt"></div>
  <label for="ssid">Nom du reseau (SSID)</label>
  <input id="ssid" autocapitalize="off" autocorrect="off" spellcheck="false" placeholder="MaBox-1234">
  <label for="pass">Mot de passe</label>
  <input id="pass" type="password" autocomplete="off" placeholder="mot de passe WiFi">
  <div class="row mt"><button id="go">Se connecter</button>
    <label class="grow" style="margin:0"><input type="checkbox" id="show" style="width:auto">
      <span class="kv"> afficher</span></label></div>
  <div class="msg" id="m1"></div>
</div>

<div class="card">
  <h2>2. Ou rester en mode point d'acces</h2>
  <div class="kv">Le flipper n'a pas besoin d'internet. Vous pouvez garder son hotspot en
    permanence&nbsp;: connectez-vous simplement au reseau <b id="apn2">&hellip;</b> pour utiliser
    l'interface. Ce choix est memorise, la carte ne redemandera plus rien.</div>
  <button class="ghost mt" id="apo">Rester en mode point d'acces</button>
  <div class="msg" id="m2"></div>
</div>

<div class="card">
  <h2>Point d'acces &mdash; securite</h2>
  <div class="kv">Reseau <b id="apn" class="mono">&hellip;</b><br>Mot de passe
    <b id="app" class="mono">&hellip;</b></div>
  <div class="msg w on" id="mdef">Mot de passe d'usine : changez-le, sinon n'importe qui a portee
    peut reprogrammer la carte.</div>
  <details class="mt"><summary>Changer le mot de passe du point d'acces</summary>
    <label for="np">Nouveau mot de passe (8 caracteres minimum)</label>
    <input id="np" type="text" autocomplete="off" autocapitalize="off" spellcheck="false">
    <button class="warn mt" id="sap">Enregistrer</button>
    <div class="hint">Le hotspot redemarre : reconnectez-vous avec le nouveau mot de passe.</div>
  </details>
  <div class="msg" id="m3"></div>
</div>

<div class="card">
  <h2>Reinitialisation</h2>
  <div class="kv">Oublie le reseau enregistre et relance cet assistant.
    Sans ordinateur&nbsp;: maintenez le bouton <b>BOOT</b> de la carte ESP32 pendant
    <b>5&nbsp;secondes</b>, carte allumee.</div>
  <button class="bad mt" id="rst">Oublier le reseau</button>
  <div class="msg" id="m4"></div>
</div>

<div class="hint">Une fois connecte a votre box, l'interface reste accessible sur
  <span class="mono">http://gottfa.local/</span> ou a l'adresse IP affichee ci-dessus.</div>
</div><script>
var $=function(s){return document.getElementById(s)};
function esc(s){return String(s).replace(/[&<>"]/g,function(c){
  return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function msg(id,cls,txt){var e=$(id);e.className='msg on '+cls;e.innerHTML=txt}
function hide(id){$(id).className='msg'}
function post(u,b){return fetch(u,{method:'POST',headers:{
  'Content-Type':'application/x-www-form-urlencoded'},body:b||''}).then(function(r){return r.json()})}

/* ---- status: polled, also carries the outcome of a connection attempt ---- */
var busy=false;
function paint(j){
  $('apn').textContent=j.apssid; $('apn2').textContent=j.apssid; $('app').textContent=j.appass;
  $('mdef').className=j.apdefault?'msg w on':'msg';
  var l='Mode <b>'+esc(j.mode)+'</b><br>Adresse <b class="mono">'+esc(j.ip)+'</b>';
  if(j.sta) l+='<br>Connecte a <b>'+esc(j.ssid)+'</b> ('+j.rssi+' dBm)';
  else if(j.ssid) l+='<br>Reseau enregistre <b>'+esc(j.ssid)+'</b> (non joignable)';
  else if(j.aponly) l+='<br>Mode point d\'acces permanent';
  else l+='<br><span style="color:var(--warn)">Aucun reseau configure</span>';
  $('st').innerHTML=l;
  busy=j.busy;
  $('go').disabled=busy; $('rescan').disabled=busy;
  if(j.busy) msg('m1','i','<span class="spin"></span> Connexion a <b>'+esc(j.trying)+'</b>&hellip;');
  else if(j.result=='ok') msg('m1','o','Connecte&nbsp;! Adresse <b class="mono">'+esc(j.ip)+
    '</b>. Le point d\'acces va s\'eteindre : rejoignez votre WiFi habituel puis ouvrez '+
    '<b class="mono">http://gottfa.local/</b> ou <b class="mono">http://'+esc(j.ip)+'/</b>.');
  else if(j.result=='fail') msg('m1','e','Echec&nbsp;: '+esc(j.msg)+
    '<br>Le point d\'acces reste actif, vous pouvez reessayer.');
}
function status(){return fetch('/wifi/status').then(function(r){return r.json()}).then(paint)
  .catch(function(){})}

/* ---- scan: the ESP answers instantly; the actual scan runs in the main loop ---- */
function bars(r){var n=r>=-55?4:r>=-67?3:r>=-78?2:1,h='';
  for(var i=1;i<=4;i++)h+='<i class="'+(i<=n?'on':'')+'"></i>';return '<span class="bars">'+h+'</span>'}
function scan(force){
  $('nets').innerHTML='<div class="kv"><span class="spin"></span> recherche&hellip;</div>';
  var tries=0,u=force?'/wifi/scan?force=1':'/wifi/scan';
  (function poll(){
    fetch(u).then(function(r){return r.json()}).then(function(j){
      u='/wifi/scan';
      if(j.scanning&&tries++<20){setTimeout(poll,900);return}
      if(!j.nets||!j.nets.length){$('nets').innerHTML=
        '<div class="kv">Aucun reseau trouve. Rapprochez la carte de la box, puis relancez.</div>';return}
      var h='';
      for(var i=0;i<j.nets.length;i++){var n=j.nets[i];
        h+='<button class="net" data-s="'+esc(n.s)+'">'+bars(n.r)+
           '<span class="nm">'+esc(n.s)+'</span><span class="ic">'+(n.e?'&#128274;':'ouvert')+'</span></button>'}
      $('nets').innerHTML=h;
      var b=$('nets').getElementsByClassName('net');
      for(var k=0;k<b.length;k++)b[k].onclick=function(){
        $('ssid').value=this.getAttribute('data-s');$('pass').focus()}
    }).catch(function(){$('nets').innerHTML='<div class="kv">Scan indisponible.</div>'})
  })()
}

$('rescan').onclick=function(){scan(1)};
$('show').onchange=function(){$('pass').type=this.checked?'text':'password'};
$('go').onclick=function(){
  var s=$('ssid').value.trim();
  if(!s){msg('m1','e','Choisissez un reseau ou saisissez son nom.');return}
  msg('m1','i','<span class="spin"></span> Connexion&hellip;');busy=true;$('go').disabled=true;
  post('/wifi/connect','ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent($('pass').value))
    .then(function(j){if(!j.ok)msg('m1','e',esc(j.err||'refuse'))})
    .catch(function(){msg('m1','i','<span class="spin"></span> Connexion en cours&hellip;')})
};
$('apo').onclick=function(){
  post('/wifi/aponly').then(function(){
    msg('m2','o','Mode point d\'acces permanent enregistre. Rien d\'autre a faire : '+
      'rejoignez ce reseau quand vous voulez jouer.')})
};
$('sap').onclick=function(){
  var p=$('np').value;
  if(p.length<8){msg('m3','e','8 caracteres minimum.');return}
  post('/wifi/appass','pass='+encodeURIComponent(p)).then(function(j){
    if(j.ok)msg('m3','o','Enregistre. Le point d\'acces redemarre dans quelques secondes.');
    else msg('m3','e',esc(j.err||'refuse'))})
};
$('rst').onclick=function(){
  if(!confirm('Oublier le reseau enregistre et revenir a l\'assistant ?'))return;
  post('/wifi/forget').then(function(){msg('m4','w','Efface. Le point d\'acces reste actif.')})
};

status();scan();setInterval(status,1500);
</script></body></html>)HTML";
