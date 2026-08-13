#pragma once

#include <Arduino.h>

const char WEB_INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>RemoteBox</title>
  <style>
    :root{color-scheme:light dark;--bg:#eef3f7;--card:#fff;--surface:#f5f7fa;--line:#d8e1e8;--text:#17212b;--muted:#667483;--accent:#1677ff;--ok:#14804a;--bad:#c93745}
    @media(prefers-color-scheme:dark){:root{--bg:#0b131d;--card:#121f2d;--surface:#172838;--line:#2b4053;--text:#edf5fb;--muted:#9aabba;--accent:#62a9ff;--ok:#55cf8b;--bad:#ff7783}}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}.wrap{width:min(960px,calc(100% - 24px));margin:auto;padding:24px 0 48px}.top{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:18px}.brand{font-size:21px;font-weight:800;letter-spacing:.08em}.live{display:flex;align-items:center;gap:7px;color:var(--muted);font-size:13px}.dot{width:9px;height:9px;border-radius:50%;background:var(--bad)}.dot.ok{background:var(--ok)}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:13px}.card,.editor{padding:18px;border:1px solid var(--line);border-radius:15px;background:var(--card);box-shadow:0 10px 30px #1831470d}.wide{grid-column:span 2}.full{grid-column:1/-1}.label{margin-bottom:6px;color:var(--muted);font-size:11px;letter-spacing:.12em;text-transform:uppercase}.value{font-size:23px;font-weight:750;overflow-wrap:anywhere}.sub{margin-top:3px;color:var(--muted);font-size:13px}.editor{margin-top:14px}.editor-head{display:flex;align-items:center;justify-content:space-between;gap:12px}.editor h2{margin:0;font-size:17px}.editor p{margin:4px 0 0;color:var(--muted);font-size:13px}textarea{display:block;width:100%;min-height:420px;margin-top:14px;padding:14px;resize:vertical;border:1px solid var(--line);border-radius:11px;outline:0;background:var(--surface);color:var(--text);font:13px/1.55 Consolas,"Cascadia Code",monospace;tab-size:2}textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px #1677ff22}.actions{display:flex;flex-wrap:wrap;gap:9px;margin-top:12px}button{padding:8px 13px;border:1px solid var(--line);border-radius:9px;background:var(--surface);color:var(--text);font:inherit;font-weight:650;cursor:pointer}button.primary{border-color:var(--accent);background:var(--accent);color:#fff}button:disabled{opacity:.55;cursor:wait}.msg{display:none;margin-top:11px;padding:9px 11px;border-radius:9px;background:var(--surface)}.msg.show{display:block}.msg.ok{color:var(--ok)}.msg.bad{color:var(--bad)}
    @media(max-width:700px){.grid{grid-template-columns:1fr 1fr}.wide{grid-column:1/-1}}@media(max-width:480px){.grid{grid-template-columns:1fr}.card,.wide{grid-column:1/-1}.wrap{padding-top:16px}}
  </style>
</head>
<body>
<main class="wrap">
  <header class="top"><div class="brand">REMOTEBOX</div><div class="live"><span class="dot" id="dot"></span><span id="live">正在连接</span></div></header>
  <section class="grid">
    <article class="card"><div class="label">Device</div><div class="value" id="name">--</div><div class="sub" id="id">--</div></article>
    <article class="card wide"><div class="label">Network</div><div class="value" id="ip">--</div><div class="sub" id="network">--</div></article>
    <article class="card"><div class="label">Battery</div><div class="value" id="battery">--</div><div class="sub" id="voltage">--</div></article>
    <article class="card"><div class="label">ESP-NOW</div><div class="value" id="channel">--</div><div class="sub" id="command">--</div></article>
    <article class="card"><div class="label">Target</div><div class="value" id="target">--</div><div class="sub" id="mdns">--</div></article>
    <article class="card full"><div class="label">Runtime</div><div class="value" id="uptime">--</div><div class="sub" id="runtime">--</div></article>
  </section>
  <section class="editor">
    <div class="editor-head"><div><h2>config.json</h2><p>保存前会同时执行浏览器 JSON 语法校验和设备端字段校验。</p></div></div>
    <textarea id="config" spellcheck="false" aria-label="config.json"></textarea>
    <div class="msg" id="msg"></div>
    <div class="actions"><button id="format">格式化 / 校验</button><button id="reload">重新载入</button><button class="primary" id="save">保存配置</button><button id="reboot">重启设备</button></div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id),msg=(text,ok)=>{const e=$('msg');e.textContent=text;e.className='msg show '+(ok?'ok':'bad')};
const bytes=n=>n<1024?n+' B':(n/1024).toFixed(1)+' KiB',duration=s=>{s=Math.max(0,Number(s)||0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'天 ':'')+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')};
async function request(url,options){const r=await fetch(url,{cache:'no-store',...options});const text=await r.text();let data={};try{data=text?JSON.parse(text):{}}catch{throw Error(text||('HTTP '+r.status))}if(!r.ok)throw Error(data.error||('HTTP '+r.status));return data}
async function refresh(){try{const d=await request('/api/status');$('dot').classList.add('ok');$('live').textContent='已连接';$('name').textContent=d.name;$('id').textContent='ID: '+d.id;$('ip').textContent=d.ip;$('network').textContent=d.ssid+' · '+d.rssi+' dBm · Wi-Fi CH '+d.wifiChannel;$('battery').textContent=d.batteryPercent+'%';$('voltage').textContent=d.batteryMillivolts+' mV';$('channel').textContent='CH '+d.espnowChannel;$('command').textContent='CMD: '+d.command;$('target').textContent=d.target;$('mdns').textContent=d.mdns+'.local · ESPOTA :3232';$('uptime').textContent=duration(d.uptime);$('runtime').textContent='可用内存 '+bytes(d.freeHeap)+' · LittleFS '+bytes(d.fsUsed)+' / '+bytes(d.fsTotal)+' · '+(d.otaActive?'OTA 进行中':'OTA 就绪')}catch(e){$('dot').classList.remove('ok');$('live').textContent='连接中断'}}
async function loadConfig(){try{const r=await fetch('/api/config',{cache:'no-store'}),text=await r.text();if(!r.ok){let e=text;try{e=JSON.parse(text).error||text}catch{}throw Error(e)}$('config').value=text;msg('配置已载入',true)}catch(e){msg(e.message,false)}}
function formatConfig(){try{$('config').value=JSON.stringify(JSON.parse($('config').value),null,2)+'\n';msg('JSON 语法正确；保存时还会校验字段和值域。',true)}catch(e){msg('JSON 语法错误：'+e.message,false)}}
async function saveConfig(){let parsed;try{parsed=JSON.parse($('config').value)}catch(e){msg('JSON 语法错误：'+e.message,false);return}const b=$('save');b.disabled=true;try{const d=await request('/api/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(parsed,null,2)});$('config').value=JSON.stringify(parsed,null,2)+'\n';msg(d.wifiChanged?'保存成功，Wi-Fi 将用新配置重新连接。':'保存并校验成功。',true);refresh()}catch(e){msg('保存失败：'+e.message,false)}finally{b.disabled=false}}
async function reboot(){if(!confirm('确定要重启 RemoteBox？'))return;try{await request('/api/reboot',{method:'POST'});msg('设备正在重启，请稍候重新打开页面。',true)}catch(e){msg(e.message,false)}}
$('format').onclick=formatConfig;$('reload').onclick=loadConfig;$('save').onclick=saveConfig;$('reboot').onclick=reboot;refresh();loadConfig();setInterval(refresh,3000);
</script>
</body>
</html>)HTML";
