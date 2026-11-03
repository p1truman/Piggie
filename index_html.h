#pragma once
#include <Arduino.h>

const char PAGE[] PROGMEM = R"HUTCHPAGE(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Guinea Pig Hutch</title>
<style>
:root{
  --bg:#141c26; --card:#1f2b39; --card2:#263647; --line:#33465b;
  --text:#e8eef5; --muted:#8fa3b8; --accent:#e6843a;
  --ok:#43b581; --warn:#f0b232; --bad:#ed4245; --cool:#4fc3f7;
}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
  background:var(--bg);color:var(--text);line-height:1.4}
header{position:sticky;top:0;z-index:10;display:flex;justify-content:space-between;
  align-items:center;padding:12px 16px;background:#0f161e;border-bottom:1px solid var(--line)}
header h1{font-size:1.1rem;margin:0}
.conn{font-size:.8rem;color:var(--muted);display:flex;align-items:center;gap:6px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--bad)}
.dot.on{background:var(--ok)}
main{max-width:1000px;margin:0 auto;padding:14px;display:grid;gap:14px;
  grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px}
.card.wide{grid-column:1/-1}
.card h2{margin:0 0 12px;font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted)}
.temps{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.temp{background:var(--card2);border-radius:12px;padding:14px;text-align:center}
.temp .lbl{font-size:.8rem;color:var(--muted)}
.temp .val{font-size:2.3rem;font-weight:700;margin-top:4px}
.temp.cold .val{color:var(--cool)}
.temp.hot .val{color:var(--bad)}
.temp.err .val{color:var(--warn)}
.badges{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px}
.pill{padding:4px 12px;border-radius:999px;font-size:.8rem;font-weight:600;background:var(--card2);color:var(--muted)}
.pill.on{background:rgba(67,181,129,.18);color:var(--ok)}
.pill.heat{background:rgba(230,132,58,.2);color:var(--accent)}
.seg{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:14px}
.seg2{grid-template-columns:repeat(2,1fr)}
button{font:inherit;font-weight:600;padding:11px 10px;border:1px solid var(--line);border-radius:10px;
  background:var(--card2);color:var(--text);cursor:pointer;transition:.15s}
button:hover{border-color:var(--accent)}
button:active{transform:scale(.97)}
button.active{background:var(--accent);border-color:var(--accent);color:#fff}
button.primary{background:#35597d;border-color:#35597d;width:100%;margin-top:6px}
.form{display:grid;grid-template-columns:1fr auto;gap:8px 12px;align-items:center;margin-bottom:6px}
.form label{color:var(--muted);font-size:.9rem}
input[type=number]{width:96px;padding:9px;border-radius:8px;border:1px solid var(--line);
  background:#121a24;color:var(--text);font:inherit;text-align:right}
input[type=number]:focus{outline:2px solid var(--accent);border-color:transparent}
input[type=checkbox]{width:22px;height:22px;accent-color:var(--accent)}
.sub{font-size:.8rem;color:var(--muted);margin:2px 0 10px}
.chartbox{position:relative;height:300px}
.chartmsg{color:var(--muted);text-align:center;padding-top:110px}
footer{text-align:center;color:var(--muted);font-size:.75rem;padding:6px 0 24px}
#toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%) translateY(80px);
  background:#0f161e;border:1px solid var(--line);padding:10px 18px;border-radius:10px;
  opacity:0;transition:.25s;z-index:20}
#toast.show{transform:translateX(-50%) translateY(0);opacity:1}
#toast.err{border-color:var(--bad);color:var(--bad)}
@media(max-width:420px){.temp .val{font-size:1.9rem}}
</style>
</head>
<body>
<header>
  <h1>Guinea Pig Hutch</h1>
  <div class="conn"><span class="dot" id="dot"></span><span id="updated">connecting</span></div>
</header>

<main>
  <section class="card wide">
    <h2>Temperatures</h2>
    <div class="temps">
      <div class="temp" id="floorBox"><div class="lbl">Floor</div><div class="val" id="floor">--</div></div>
      <div class="temp" id="ambBox"><div class="lbl">Ambient</div><div class="val" id="ambient">--</div></div>
    </div>
  </section>

  <section class="card">
    <h2>Heat pad</h2>
    <div class="badges"><span class="pill" id="heatPill">--</span></div>
    <div class="seg">
      <button id="m_off" onclick="setMode('off')">Off</button>
      <button id="m_on" onclick="setMode('on')">On</button>
      <button id="m_auto" onclick="setMode('auto')">Auto</button>
    </div>
    <div class="sub">Auto mode uses the floor sensor</div>
    <div class="form">
      <label for="onBelow">Turn on below (C)</label><input id="onBelow" type="number" step="0.5">
      <label for="offAbove">Turn off above (C)</label><input id="offAbove" type="number" step="0.5">
    </div>
    <button class="primary" onclick="setRange()">Save auto range</button>
  </section>

  <section class="card">
    <h2>Camera</h2>
    <div class="badges"><span class="pill" id="camPill">--</span></div>
    <div class="seg seg2">
      <button onclick="setCam(1)">Turn on</button>
      <button onclick="setCam(0)">Turn off</button>
    </div>
    <div class="sub">Day/night schedule (hours, 0-23)</div>
    <div class="form">
      <label for="schEn">Schedule enabled</label><input id="schEn" type="checkbox">
      <label for="schDay">On at hour</label><input id="schDay" type="number" min="0" max="23">
      <label for="schNight">Off at hour</label><input id="schNight" type="number" min="0" max="23">
    </div>
    <button class="primary" onclick="saveSchedule()">Save schedule</button>
  </section>

  <section class="card wide">
    <h2>Alert thresholds</h2>
    <div class="temps">
      <div class="form" style="margin:0">
        <label for="fc">Floor too cold (C)</label><input id="fc" type="number" step="0.5">
        <label for="fh">Floor too hot (C)</label><input id="fh" type="number" step="0.5">
      </div>
      <div class="form" style="margin:0">
        <label for="ac">Ambient too cold (C)</label><input id="ac" type="number" step="0.5">
        <label for="ah">Ambient too hot (C)</label><input id="ah" type="number" step="0.5">
      </div>
    </div>
    <button class="primary" onclick="saveAlerts()">Save alert thresholds</button>
  </section>

  <section class="card wide">
    <h2>Last 24 hours</h2>
    <div class="chartbox"><canvas id="chart"></canvas><div class="chartmsg" id="chartmsg" style="display:none"></div></div>
  </section>
</main>
<footer>Refreshes every 3 seconds</footer>
<div id="toast"></div>

<script src="https://cdn.jsdelivr.net/npm/chart.js" onerror="window.noChart=true"></script>
<script>
let init=false, chart=null, limits={fc:0,fh:99,ac:0,ah:99};
function $(id){return document.getElementById(id);}
function fmt(v){return (v===null||v===undefined)?"ERR":Number(v).toFixed(1)+" C";}
function toast(msg,err){
  const t=$('toast'); t.textContent=msg; t.className='show'+(err?' err':'');
  clearTimeout(window._tt); window._tt=setTimeout(function(){t.className='';},2200);
}
async function fetchJSON(u){
  const r=await fetch(u); if(!r.ok) throw new Error("HTTP "+r.status); return await r.json();
}
async function post(u,okMsg){
  try{
    const r=await fetch(u,{method:'POST'});
    if(!r.ok){toast(await r.text(),true);}else{toast(okMsg||'Saved');}
  }catch(e){toast('Connection error',true);}
  refreshStatus();
}
function tempClass(v,cold,hot){
  if(v===null||v===undefined) return 'temp err';
  if(v<cold) return 'temp cold';
  if(v>hot) return 'temp hot';
  return 'temp';
}
async function refreshStatus(){
  try{
    const s=await fetchJSON('/api/status');
    $('floor').textContent=fmt(s.floor);
    $('ambient').textContent=fmt(s.ambient);
    $('floorBox').className=tempClass(s.floor,s.floorCold,s.floorHot);
    $('ambBox').className=tempClass(s.ambient,s.ambCold,s.ambHot);
    const hp=$('heatPill'); hp.textContent='Heat '+(s.heat?'ON':'OFF')+' ('+s.mode+')';
    hp.className='pill'+(s.heat?' heat':'');
    const cp=$('camPill'); cp.textContent='Camera '+(s.cam?'ON':'OFF');
    cp.className='pill'+(s.cam?' on':'');
    ['off','on','auto'].forEach(function(m){$('m_'+m).classList.toggle('active',s.mode===m);});
    if(!init){
      $('onBelow').value=s.onBelow; $('offAbove').value=s.offAbove;
      $('schEn').checked=s.camSchedEn; $('schDay').value=s.camDay; $('schNight').value=s.camNight;
      $('fc').value=s.floorCold; $('fh').value=s.floorHot;
      $('ac').value=s.ambCold; $('ah').value=s.ambHot;
      init=true;
    }
    $('dot').className='dot on';
    $('updated').textContent=new Date().toLocaleTimeString();
  }catch(e){
    $('dot').className='dot'; $('updated').textContent='offline';
  }
}
function setMode(m){post('/api/mode?v='+encodeURIComponent(m),'Heat mode: '+m);}
function setCam(v){post('/api/cam?v='+v,'Camera '+(v?'on':'off'));}
function setRange(){
  post('/api/range?lo='+encodeURIComponent($('onBelow').value)+'&hi='+encodeURIComponent($('offAbove').value),'Auto range saved');
}
function saveSchedule(){
  post('/api/schedule?en='+($('schEn').checked?1:0)+'&day='+encodeURIComponent($('schDay').value)+'&night='+encodeURIComponent($('schNight').value),'Schedule saved');
}
function saveAlerts(){
  post('/api/alerts?fc='+encodeURIComponent($('fc').value)+'&fh='+encodeURIComponent($('fh').value)+
       '&ac='+encodeURIComponent($('ac').value)+'&ah='+encodeURIComponent($('ah').value),'Alert thresholds saved');
}
async function loadHistory(){
  if(window.noChart||typeof Chart==='undefined'){
    $('chart').style.display='none';
    const m=$('chartmsg'); m.style.display='block'; m.textContent='Chart needs internet access (Chart.js not loaded)';
    return;
  }
  try{
    const h=await fetchJSON('/api/history');
    const labels=h.ts.map(function(t){return new Date(t*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});});
    if(chart) chart.destroy();
    chart=new Chart($('chart'),{
      type:'line',
      data:{labels:labels,datasets:[
        {label:'Ambient C',data:h.amb,borderColor:'#4fc3f7',spanGaps:true,yAxisID:'y',pointRadius:0,borderWidth:2},
        {label:'Floor C',data:h.floor,borderColor:'#ffb74d',spanGaps:true,yAxisID:'y',pointRadius:0,borderWidth:2},
        {label:'Heat',data:h.heat,borderColor:'#ed4245',stepped:true,yAxisID:'y2',pointRadius:0,borderWidth:1},
        {label:'Camera',data:h.cam,borderColor:'#43b581',stepped:true,yAxisID:'y2',pointRadius:0,borderWidth:1}
      ]},
      options:{
        animation:false,responsive:true,maintainAspectRatio:false,
        plugins:{legend:{labels:{color:'#e8eef5'}}},
        scales:{
          x:{ticks:{color:'#8fa3b8',maxTicksLimit:8},grid:{color:'#2a3b4d'}},
          y:{type:'linear',position:'left',ticks:{color:'#8fa3b8'},grid:{color:'#2a3b4d'}},
          y2:{type:'linear',position:'right',min:0,max:1,ticks:{color:'#8fa3b8',stepSize:1},grid:{drawOnChartArea:false}}
        }
      }
    });
  }catch(e){}
}
async function boot(){
  await refreshStatus();
  loadHistory();
  setInterval(refreshStatus,3000);
  setInterval(loadHistory,60000);
}
window.addEventListener('load',boot);
</script>
</body>
</html>
)HUTCHPAGE";