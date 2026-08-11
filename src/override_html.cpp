// override_html.cpp
// Extracted from src/main.cpp: the PROGMEM raw string that holds the entire
// Override phone UI (HTML + CSS + JS). Kept in its own translation unit so
// the ~275-line web app no longer sits between the WebSocket ingress code
// and the telemetry builder in main.cpp.
//
// Design DNA (plan §5):
//   layout = centered_stack ; hero motif = dashboard_zoom
//   accent = Spotify-green primary (#1db954) + warm-purple restraint (#8c5cff)
//   form  = rounded-soft (radius-12 cards, radius-8 buttons)
//   ≤ 4 cards on phone, body ≥ 14pt, tap targets ≥ 44px,
//   1 primary + 1 ghost CTA above the fold, outcome-led copy.
// Binary WS protocol — single /ws connection, no polling.
#include "override_html.h"

const char OVERRIDE_HTML[] PROGMEM = R"OD(<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,user-scalable=no,viewport-fit=cover">
<title>Override</title>
<style>
/* Semantic tokens only. Any hex or rgba that appears in a rule below should
   trace back to one of these — a palette swap should ride a single :root
   override, not a scattergun edit across the CSS. See DSGN-10. */
:root{color-scheme:dark;
  --bg:#0a0a0a;--surf:#141414;--surf2:#1c1c1c;--surf-lo:#0d0d0d;--surf-hi:#1a1a1a;
  --text:#f3f3f3;--muted:#9aa0a6;--on-accent:#0a0a0a;
  --accent:#1db954;--accent-lo:#0e5a2a;--accent-hi:#3ad975;
  --danger:#ef4444;--warn:#d29922;
  --accent-a12:rgba(29,185,84,.12);--accent-a16:rgba(29,185,84,.16);
  --accent-a18:rgba(29,185,84,.18);--accent-a45:rgba(29,185,84,.45);
  --overlay-5:rgba(255,255,255,.05);--overlay-6:rgba(255,255,255,.06);
  --overlay-10:rgba(255,255,255,.1);--scrim:rgba(0,0,0,.7);--scrim-lo:rgba(0,0,0,.55)}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;-webkit-touch-callout:none}
html,body{margin:0;background:var(--bg);color:var(--text);font:14px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-user-select:none;user-select:none;overscroll-behavior:none}
body{padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}
h1{font:600 18px/1.2 system-ui;letter-spacing:-.02em;margin:0}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted);display:inline-block;margin-right:6px;vertical-align:middle;transition:background .18s}
.dot.live{background:var(--accent);box-shadow:0 0 0 3px var(--accent-a18)}
.badge{display:inline-flex;align-items:center;padding:4px 10px;border-radius:999px;font:600 11px/1.5 system-ui;background:var(--surf2);color:var(--muted)}
.badge.charging{background:var(--accent-a16);color:var(--accent)}
.card{background:var(--surf);border:1px solid var(--surf2);border-radius:12px;padding:12px}
button{appearance:none;border:0;border-radius:8px;font:600 14px/1 system-ui;cursor:pointer;min-height:44px;transition:transform .18s cubic-bezier(.2,.8,.2,1);color:var(--text)}
button:active{transform:scale(.97)}
.primary{background:var(--accent);color:var(--on-accent)}
/* The one dangerous control on the page must not share the colour of every
   positive state (live dot, active mood, battery bar, stick thumbs). STOP
   keeps its unique shape and position and now gets a unique colour too.
   `.stopped` is the latched state: the whole surface reports it, not just
   the button, and it is text-labelled as well as coloured. */
.danger{background:var(--danger);color:var(--text)}
.dot.stopped{background:var(--danger)}
body.stopped .stick-pad,body.stopped .vstick{opacity:.45;filter:grayscale(1)}
body.stopped .stopBig{outline:2px solid var(--text);outline-offset:-4px}
.ghost{background:transparent;color:var(--text);border:1px solid var(--surf2)}
.bigV{font:600 24px/1 system-ui;letter-spacing:-.02em}
.bigV small{font:500 12px/1 system-ui;color:var(--muted);margin-left:2px}
.pct{font:600 13px/1 system-ui;color:var(--muted)}
.bar{height:4px;background:var(--surf2);border-radius:2px;margin-top:8px;overflow:hidden}
.bar>div{height:100%;background:var(--accent);width:0;transition:width .3s ease,background .18s}
/* DSGN-5, DSGN-9: state must never be conveyed by colour alone. The battery
   badge grows a leading "!" glyph when we drop below the danger threshold, so
   a monochrome screenshot or a colour-blind operator still reads "low". */
body.batLow .pct::before{content:"! ";color:var(--danger);font-weight:800}
body.batLow #chg{border:1px solid var(--danger)}
.battline{display:flex;align-items:baseline;gap:8px;flex-wrap:wrap}
/* Main-view expression row. 44 px min-height clears the Fitts baseline; the
   sheet override below then becomes decoration, not correction. See DSGN-4.
   The global lowercase text-transform that used to live here moved down onto
   `.sheet .face` — the sheet tags (mood/gesture/sound) still read best
   lowercase, but the main-view buttons are named actions and should stay
   capitalised as written. See DSGN-12. */
.faces{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.face{appearance:none;background:var(--surf2);border:1px solid transparent;color:var(--text);border-radius:8px;padding:10px 4px;font:500 12px/1.2 system-ui;cursor:pointer;min-height:44px;transition:border-color .18s,background .18s}
/* Active-mood state carries shape (leading dot) + weight, in addition to
   colour. Colour-blind and printed-monochrome operators can still read
   which mood is active. See DSGN-5. */
.face.active{border-color:var(--accent);background:var(--accent-a12);color:var(--accent);font-weight:700}
.face.active::before{content:"\25CF\00a0";color:var(--accent)}
.stick-pad{position:relative;background:radial-gradient(circle at center,var(--surf-hi) 0%,var(--surf-lo) 70%);border:1px solid var(--surf2);border-radius:50%;touch-action:none;overflow:hidden}
.stick-pad::before{content:"";position:absolute;inset:8px;border-radius:50%;border:1px dashed var(--overlay-6);pointer-events:none}
.thumb{position:absolute;width:64px;height:64px;border-radius:50%;background:radial-gradient(circle at 35% 35%,var(--accent-hi) 0%,var(--accent) 50%,var(--accent-lo) 100%);box-shadow:0 6px 18px var(--accent-a45),inset 0 0 0 1px var(--overlay-10);left:50%;top:50%;transform:translate(-50%,-50%);transition:transform .14s cubic-bezier(.2,.8,.2,1);pointer-events:none}
.stick-pad.live .thumb{transition:none}
.vstick{position:relative;background:linear-gradient(180deg,var(--surf-hi),var(--surf-lo));border:1px solid var(--surf2);border-radius:16px;touch-action:none;overflow:hidden;flex:1;min-height:0}
.vstick::before{content:"";position:absolute;left:50%;top:8px;bottom:8px;width:4px;background:var(--overlay-5);border-radius:2px;transform:translateX(-50%);pointer-events:none}
.vstick::after{content:"";position:absolute;left:8px;right:8px;top:50%;height:1px;background:var(--overlay-6);pointer-events:none}
.vthumb{position:absolute;left:50%;top:50%;width:64px;height:64px;border-radius:50%;background:radial-gradient(circle at 35% 35%,var(--accent-hi) 0%,var(--accent) 50%,var(--accent-lo) 100%);box-shadow:0 6px 18px var(--accent-a45),inset 0 0 0 1px var(--overlay-10);transform:translate(-50%,-50%);transition:transform .14s cubic-bezier(.2,.8,.2,1);pointer-events:none}
.vstick.live .vthumb{transition:none}
.vlabel{position:absolute;left:50%;transform:translateX(-50%);font:700 9px/1 system-ui;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);pointer-events:none}
.vlabel.top{top:8px}
.vlabel.bot{bottom:8px}
.vval{position:absolute;left:50%;bottom:24px;transform:translateX(-50%);font:600 14px/1 system-ui;color:var(--muted);font-variant-numeric:tabular-nums;pointer-events:none}
/* Drive-state overlay. This is the fix for DSGN-3: the operator is looking
   at their thumb, not the header pill, so the pad has to carry the mode.
   `paintDriveState()` sets `.stateLbl` textContent for LOCKED, WAIT NEUTRAL,
   SAFE OFF, LINK STALE, STOPPED, and "reconnecting…" for a down socket,
   then toggles `.on` on the label. When drive is ARMED and the link is up
   the label is empty and hidden - the pad is fully live. */
.stateLbl{position:absolute;inset:0;display:none;align-items:center;justify-content:center;padding:8px;background:var(--scrim-lo);color:var(--text);font:700 12px/1.2 system-ui;text-transform:uppercase;letter-spacing:.06em;text-align:center;border-radius:inherit;pointer-events:none}
.stateLbl.on{display:flex}
body[data-drive="LOCKED"] .stick-pad,body[data-drive="LOCKED"] .vstick,
body[data-drive="WAIT NEUTRAL"] .stick-pad,body[data-drive="WAIT NEUTRAL"] .vstick,
body[data-link="down"] .stick-pad,body[data-link="down"] .vstick{opacity:.55}
body[data-drive="LOCKED"] .thumb,body[data-drive="LOCKED"] .vthumb,
body[data-drive="WAIT NEUTRAL"] .thumb,body[data-drive="WAIT NEUTRAL"] .vthumb,
body[data-link="down"] .thumb,body[data-link="down"] .vthumb{background:var(--surf2);box-shadow:none}
.subhead{font:600 11px/1 system-ui;color:var(--muted);text-transform:uppercase;letter-spacing:.05em;margin:0 0 8px}
/* NOTE: `up` is still used by the .sheet panel animation below. The #hint
   rules that also used it were removed - no #hint element exists. */
@keyframes up{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:none}}
.sheet{position:fixed;inset:0;background:var(--scrim);z-index:30;display:none;align-items:flex-end;justify-content:center}
.sheet.show{display:flex}
.sheet .p{background:var(--surf);border-radius:16px 16px 0 0;width:100%;max-width:520px;padding:16px;padding-bottom:max(16px,env(safe-area-inset-bottom));animation:up .24s cubic-bezier(.2,.8,.2,1)}
.sheet .faces{grid-template-columns:repeat(4,1fr);gap:8px}
/* Mood tags and audition names (happy/sad/curious…) read best in lowercase.
   Scoped here rather than globally so the main-view Face…/Sound…/Move…
   buttons keep the capitalisation their labels were typed with. DSGN-12. */
.sheet .face{min-height:48px;font-size:12px;text-transform:lowercase}
.sheet .x{margin-top:12px;width:100%;background:var(--surf2)}
/* Tap confirmation. Faces show selection via .active, but a sound or a
   movement leaves no visible state behind, so without this a tap looks like
   nothing happened - the robot responds a beat later, off-screen. Only ever
   applied to the sheet audition grids, never the main-view opener row -
   that would mean "the robot did something" when nothing but a sheet had
   opened. See sheetGrid vs nameGrid, and DSGN-11. */
.face.picked{background:var(--accent);color:var(--on-accent)}
.sheet .hint{margin:12px 0 0;font-size:12px;color:var(--muted);text-align:center}
@media (orientation:portrait){
  body{overflow:auto;min-height:100vh}
  .grid{display:flex;flex-direction:column;gap:12px;padding:12px;max-width:520px;margin:0 auto}
  .tank{display:none!important}
  .joy-card{display:flex;flex-direction:column;align-items:center;gap:8px}
  .joy-card .stick-pad{width:min(86vw,360px);aspect-ratio:1/1}
  .joy-vals{display:flex;gap:16px;color:var(--muted);font:600 13px/1 system-ui;font-variant-numeric:tabular-nums}
  .header-l{display:flex;justify-content:space-between;align-items:baseline}
  .battline .pct,.battline .badge{margin-left:auto}
  .stopBig{width:100%;padding:16px 12px;font-size:17px}
}
@media (orientation:landscape){
  html,body{overflow:hidden;height:100vh;height:100dvh}
  /* Landscape uses a deliberately tight 6/6 spacing budget - the three
     columns plus a full-width STOP bar must fit on a 360x160 phone in
     landscape without introducing scroll. Off-scale by design; see DSGN-14. */
  .grid{display:grid;grid-template-columns:1.05fr 1.3fr 1.05fr;grid-template-rows:1fr auto;gap:6px;padding:6px;height:100%}
  .joy-card{display:none!important}
  .tank{display:flex;flex-direction:column;min-height:0}
  .center{display:flex;flex-direction:column;gap:8px;min-height:0;overflow:hidden}
  .center .card{padding:12px}
  /* STOP is the emergency control, so it spans the full width under all three
     columns rather than sitting in the centre column. Fitts: the widest target
     on screen, and always in the same place regardless of orientation. */
  .stopBig{grid-column:1/-1;width:100%;padding:14px 12px;font-size:16px}
  .header-l{display:flex;justify-content:space-between;align-items:baseline}
  }
@media (prefers-reduced-motion:reduce){*{transition:none!important;animation:none!important}}
</style></head><body>
<div class="grid">
  <div class="tank">
    <div class="vstick" id="VL">
      <span class="vlabel top">L FWD</span>
      <div class="vthumb"></div>
      <span class="vval" id="VLv">0</span>
      <span class="vlabel bot">L REV</span>
      <span class="stateLbl" aria-live="polite"></span>
    </div>
  </div>
  <div class="center">
    <div class="card header-l"><h1>Override</h1><span class="badge" id="link"><span class="dot" id="dot"></span><span id="linkT">connecting</span></span></div>
    <div class="card"><div class="battline"><div class="bigV"><span id="dsp">--</span><small>V</small></div><div class="pct"><span id="pct">--</span>%</div><span class="badge" id="chg">--</span></div><div class="bar"><div id="pctbar"></div></div></div>
    <div class="joy-card card">
      <p class="subhead">Joystick · drag to drive</p>
      <div class="stick-pad" id="JOY"><div class="thumb"></div><span class="stateLbl" aria-live="polite"></span></div>
      <div class="joy-vals"><span>L <span id="JLv">0</span></span><span>R <span id="JRv">0</span></span></div>
    </div>
    <div class="card">
      <p class="subhead">Expressions · face + move + sound</p>
      <div class="faces" id="expF"></div>
    </div>
  </div>
  <div class="tank">
    <div class="vstick" id="VR">
      <span class="vlabel top">R FWD</span>
      <div class="vthumb"></div>
      <span class="vval" id="VRv">0</span>
      <span class="vlabel bot">R REV</span>
      <span class="stateLbl" aria-live="polite"></span>
    </div>
  </div>
  <button class="danger stopBig" onclick="stopAll()">STOP</button>
</div>
<div id="moodM" class="sheet" onclick="if(event.target.id==='moodM')closeMoods()"><div class="p"><div class="faces" id="allF"></div><button class="x" onclick="closeMoods()">Close</button></div></div>
<div id="sndM" class="sheet" onclick="if(event.target.id==='sndM')closeSounds()"><div class="p"><div class="faces" id="allS"></div><p class="hint">Each tap composes a new phrase.</p><button class="x" onclick="closeSounds()">Close</button></div></div>
<div id="movM" class="sheet" onclick="if(event.target.id==='movM')closeMoves()"><div class="p"><div class="faces" id="allM"></div><button class="x" onclick="closeMoves()">Close</button></div></div>
<script>
const MOODS=[[0,"default"],[1,"bored"],[2,"angry"],[3,"happy"],[5,"think"],[6,"error"],[7,"tired"],[8,"annoyed"],[9,"sad"],[10,"surprised"],[11,"wink"],[12,"sleeping"],[13,"love"],[14,"confused"],[15,"curious"],[16,"scared"]];
/* Channel vocabularies. Indices are sent raw. MOVES indexes
   kGestureButtonNames; SOUNDS indexes MelodyGen::Mood (0=happy,1=sad,
   2=confused). Sounds are composed fresh each trigger, not fixed clips. */
const MOVES=["frustrated","excited","scared"];
const SOUNDS=["happy","sad","confused"];
const OP_CONTROL=0x01,OP_TELEMETRY=0x12,OP_TRIM=0x03,OP_MOOD=0x04,OP_GESTURE=0x05,OP_SOUND=0x06;
const $=id=>document.getElementById(id);
let ws=null,seq=0,curMood=0;
let curL=0,curR=0;
let linkLive=false,driveStatus='LOCKED';
/* STOP latch. While true, every control frame is forced to zero and the
   sticks refuse to take input. See stopAll(). */
let stopLatched=false;
let reconnectTimer=null,reconnectAttempt=0,lastRxMs=0,rxWatchdog=null;
const RECONNECT_BASE_MS=500,RECONNECT_MAX_MS=8000,RX_STALE_MS=4000;
function paintLink(){const d=$('dot'),t=$('linkT');d.classList.toggle('live',linkLive);d.classList.toggle('stopped',stopLatched);
  /* STOPPED outranks link state in the pill: while latched, that is the single
     most important thing on screen. Shown as text, not colour alone. */
  t.textContent=stopLatched?'STOPPED · tap a stick to resume':(linkLive?'live · '+driveStatus:'connecting · '+driveStatus);
  document.body.classList.toggle('stopped',stopLatched);
  /* DSGN-3: pad-visible state. `data-drive` and `data-link` gate the CSS
     that dims the pads and greys the thumbs; `paintDriveState()` writes the
     matching text into the .stateLbl overlays inside each stick, so the
     operator sees the mode without having to look at the header pill. */
  document.body.dataset.drive=driveStatus;
  document.body.dataset.link=linkLive?'live':'down';
  paintDriveState()}
function paintDriveState(){
  let msg='';
  if(stopLatched)msg='STOPPED · tap to resume';
  else if(!linkLive)msg='reconnecting…';
  else if(driveStatus==='LOCKED')msg='LOCKED';
  else if(driveStatus==='WAIT NEUTRAL')msg='centre stick';
  else if(driveStatus==='SAFE OFF')msg='SAFE OFF';
  else if(driveStatus==='LINK STALE')msg='LINK STALE';
  document.querySelectorAll('.stateLbl').forEach(el=>{el.textContent=msg;el.classList.toggle('on',!!msg)});
}
function setLink(live){linkLive=live;paintLink()}
function setDriveStatus(eligible,state){driveStatus=!eligible?'LOCKED':state===1?'WAIT NEUTRAL':state>=2&&state<=4?'ARMED':'SAFE OFF';paintLink()}
function bGrid(host,ids){host.innerHTML='';ids.forEach(id=>{const m=MOODS.find(x=>x[0]===id);if(!m)return;const b=document.createElement('button');b.className='face';b.textContent=m[1];b.dataset.id=id;b.onclick=()=>sendMood(id);host.appendChild(b)})}
function paintFace(id){curMood=id;document.querySelectorAll('.face').forEach(b=>b.classList.toggle('active',+b.dataset.id===id))}
function neutralUi(){curL=0;curR=0;document.querySelectorAll('.vthumb').forEach(t=>t.style.transform='translate(-50%,-50%)');const j=$('JOY').querySelector('.thumb');if(j)j.style.transform='translate(-50%,-50%)';paintVals()}
const ACCESS_TOKEN=new URLSearchParams(location.search).get('k')||'';
function retryWs(status){
  driveStatus=status;paintLink();
  if(reconnectTimer!==null)return;
  const delay=Math.min(RECONNECT_MAX_MS,RECONNECT_BASE_MS*Math.pow(2,reconnectAttempt));
  reconnectAttempt=Math.min(reconnectAttempt+1,5);
  reconnectTimer=setTimeout(()=>{reconnectTimer=null;openWs()},delay);
}
/* An open socket is not the same as a working link.
   The robot streams telemetry at 5 Hz, so silence means the link is dead even
   though onclose never fired. That happens whenever the firmware revokes this
   client's transport ownership: from then on our frames are ignored, and --
   deliberately -- the firmware does NOT close the socket, because closing a
   client from a stale data path was a use-after-free that reset the board.
   Without this watchdog the app sits showing "live" forever while nothing
   responds. Reported as "im connected to the network but it wont respond to
   the controller". Forcing close() drops us into the normal onclose backoff. */
function armRxWatchdog(){
  if(rxWatchdog!==null)return;
  rxWatchdog=setInterval(()=>{
    if(!ws||ws.readyState!==WebSocket.OPEN)return;
    if(lastRxMs&&Date.now()-lastRxMs>RX_STALE_MS){
      driveStatus='LINK STALE';paintLink();
      try{ws.close()}catch(e){}
    }
  },1000);
}
function openWs(){
  if(!ACCESS_TOKEN){driveStatus='AUTH REQUIRED';setLink(false);return}
  if(ws&&(ws.readyState===WebSocket.OPEN||ws.readyState===WebSocket.CONNECTING))return;
  ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws?k='+encodeURIComponent(ACCESS_TOKEN));
  ws.binaryType='arraybuffer';
  ws.onopen=()=>{reconnectAttempt=0;if(reconnectTimer!==null){clearTimeout(reconnectTimer);reconnectTimer=null}seq=0;lastRxMs=Date.now();armRxWatchdog();neutralUi();setLink(true);sendCtrl(0,0)};
  ws.onclose=e=>{neutralUi();sendCtrl(0,0);setLink(false);if(e.code===1008&&e.reason==='Forbidden'){driveStatus='AUTH FAILED';paintLink();return}retryWs(e.reason==='Owner busy'?'OWNER BUSY':e.reason==='Stale'?'STALE':e.reason==='Unavailable'?'UNAVAILABLE':'RETRYING')};
  ws.onerror=()=>{try{ws.close()}catch(e){}};
  ws.onmessage=e=>{
    const v=new DataView(e.data);
    if(v.byteLength!==30||v.getUint8(0)!==OP_TELEMETRY)return;
    lastRxMs=Date.now();
    const dsp=v.getUint16(3)/1000,pct=v.getUint8(5),mood=v.getUint8(6),chg=v.getUint8(7);
    $('dsp').textContent=dsp.toFixed(2);
    $('pct').textContent=pct;
    const bar=$('pctbar');
    bar.style.width=pct+'%';
    /* DSGN-9: three-level battery ramp with a matching non-colour cue.
       The bar changes hue; the badge grows a leading "!" so a colour-blind
       or monochrome reader still sees "low". Amber warn tone is out of the
       accent palette on purpose - a low battery is not part of the brand. */
    bar.style.background=pct<15?'var(--danger)':pct<30?'var(--warn)':'var(--accent)';
    document.body.classList.toggle('batLow',pct<15);
    const c=$('chg');c.textContent=chg?'charging':'unplugged';c.classList.toggle('charging',!!chg);
    if(mood!==curMood)paintFace(mood);
    setDriveStatus(!!v.getUint8(8),v.getUint8(9));
      }
}
function sendCtrl(l,r){
  if(!ws||ws.readyState!==1)return;
  if(ws.bufferedAmount>1024)return;
  /* While STOP is latched nothing may transmit a non-zero command, whatever
     the caller passes. This is the last gate before the wire, so a stray
     handler cannot get around it. */
  if(stopLatched){l=0;r=0}
  const b=new ArrayBuffer(4),d=new DataView(b);
  d.setUint8(0,OP_CONTROL);d.setInt8(1,l);d.setInt8(2,r);d.setUint8(3,(seq++)&0xff);
  ws.send(b);
}
function zeroBeforeClose(){neutralUi();sendCtrl(0,0)}
addEventListener('pagehide',zeroBeforeClose);
addEventListener('beforeunload',zeroBeforeClose);
setInterval(()=>sendCtrl(curL,curR),30);
function paintVals(){$('VLv').textContent=curL;$('VRv').textContent=curR;$('JLv').textContent=curL;$('JRv').textContent=curR}
function makeVStick(el,setVal){
  const thumb=el.querySelector('.vthumb');
  let active=null;
  function position(v){
    const h=el.clientHeight||200;
    const max=Math.max(0,h/2-36);
    const y=-(v/100)*max;
    thumb.style.transform='translate(-50%,calc(-50% + '+y+'px))';
  }
  function compute(e){
    const r=el.getBoundingClientRect();
    const cy=r.top+r.height/2;
    const half=Math.max(1,r.height/2-12);
    let v=(cy-e.clientY)/half*100;
    if(v>100)v=100;if(v<-100)v=-100;
    return Math.round(v);
  }
  el.addEventListener('pointerdown',e=>{
    releaseStop();   /* a deliberate new grab is the only way out of STOP */
    try{el.setPointerCapture(e.pointerId)}catch(x){}
    active=e.pointerId;el.classList.add('live');
    const v=compute(e);setVal(v);position(v);paintVals();e.preventDefault();
  });
  el.addEventListener('pointermove',e=>{
    if(e.pointerId!==active)return;
    /* Ignore movement of a finger that was already down when STOP was hit.
       Without this the latch is defeated by a twitch. */
    if(stopLatched)return;
    const v=compute(e);setVal(v);position(v);paintVals();
  });
  ['pointerup','pointercancel','lostpointercapture'].forEach(ev=>el.addEventListener(ev,e=>{
    if(e.pointerId!==active)return;
    active=null;el.classList.remove('live');setVal(0);position(0);paintVals();
  }));
  position(0);
  new ResizeObserver(()=>{if(active===null)position(0)}).observe(el);
}
makeVStick($('VL'),v=>curL=v);
makeVStick($('VR'),v=>curR=v);
function makeJoy(el,setLR){
  const thumb=el.querySelector('.thumb');
  let active=null;
  function position(x,y){
    thumb.style.transform='translate(calc(-50% + '+x+'px),calc(-50% + '+y+'px))';
  }
  function compute(e){
    const r=el.getBoundingClientRect();
    const cx=r.left+r.width/2,cy=r.top+r.height/2;
    const max=Math.max(1,r.width/2-36);
    let dx=e.clientX-cx,dy=e.clientY-cy;
    const d=Math.hypot(dx,dy);
    if(d>max){dx=dx/d*max;dy=dy/d*max}
    return {dx,dy,max};
  }
  function fire(c){
    const x=c.dx/c.max,y=-c.dy/c.max;
    let l=Math.round((y+x)*100),r=Math.round((y-x)*100);
    if(l>100)l=100;if(l<-100)l=-100;if(r>100)r=100;if(r<-100)r=-100;
    setLR(l,r);position(c.dx,c.dy);
  }
  el.addEventListener('pointerdown',e=>{
    releaseStop();   /* a deliberate new grab is the only way out of STOP */
    try{el.setPointerCapture(e.pointerId)}catch(x){}
    active=e.pointerId;el.classList.add('live');
    fire(compute(e));paintVals();e.preventDefault();
  });
  el.addEventListener('pointermove',e=>{
    if(e.pointerId!==active)return;
    /* Ignore movement of a finger that was already down when STOP was hit. */
    if(stopLatched)return;
    fire(compute(e));paintVals();
  });
  ['pointerup','pointercancel','lostpointercapture'].forEach(ev=>el.addEventListener(ev,e=>{
    if(e.pointerId!==active)return;
    active=null;el.classList.remove('live');setLR(0,0);position(0,0);paintVals();
  }));
  position(0,0);
}
makeJoy($('JOY'),(l,r)=>{curL=l;curR=r});
function stopAll(){
  /* STOP is a MODE, not a keystroke.
     It used to zero curL/curR and send one frame. That does not survive the
     posture the user is actually in when they hit it: one thumb still on a
     stick. The next pointermove reassigns curL/curR from the finger position
     and the 30 Hz sender re-arms drive within one tick, so the wheels stop for
     a single frame and then start again for no visible reason.
     Latching holds zero until the user deliberately grabs a stick again. */
  stopLatched=true;
  curL=0;curR=0;
  neutralUi();          /* re-centres every thumb so the UI matches reality */
  sendCtrl(0,0);
  paintVals();
  paintLink();
}
function releaseStop(){
  /* Cleared only by a fresh pointerdown on a stick - a deliberate new grab,
     never by a move of a finger that was already down when STOP was pressed. */
  if(!stopLatched)return;
  stopLatched=false;
  paintLink();
}
function send3(op,id){if(!ws||ws.readyState!==1)return;const b=new ArrayBuffer(3),d=new DataView(b);d.setUint8(0,op);d.setUint8(1,id);d.setUint8(2,(seq++)&0xff);ws.send(b)}
/* Picking an expression does NOT close the sheet. These are audition
   controls: the user is comparing faces, sounds and movements against each
   other, so dismissing after every tap forces a reopen for each comparison
   and makes A/B impossible. The sheet closes on Close or on the backdrop. */
function sendMood(id){paintFace(id);send3(OP_MOOD,id)}
function sendSound(id){send3(OP_SOUND,id)}
function sendMove(id){send3(OP_GESTURE,id)}
function flashPick(host,i){const b=host.children[i];if(!b)return;b.classList.add('picked');setTimeout(()=>b.classList.remove('picked'),260)}
/* nameGrid stays on the audition sheets where a tap MEANS "the robot did
   something" - the pick flash confirms exactly that. sheetGrid drops the
   flash for the main-view opener row: those buttons only open a picker,
   and reusing the audition-confirm flash there taught the operator to
   read the flash as meaningless. See DSGN-11. */
function nameGrid(host,names,fn){host.innerHTML='';names.forEach((n,i)=>{const b=document.createElement('button');b.className='face';b.textContent=n;b.onclick=()=>{flashPick(host,i);fn(i)};host.appendChild(b)})}
function sheetGrid(host,names,fn){host.innerHTML='';names.forEach((n,i)=>{const b=document.createElement('button');b.className='face';b.textContent=n;b.onclick=()=>fn(i);host.appendChild(b)})}
function openMoods(){bGrid($('allF'),MOODS.map(m=>m[0]));paintFace(curMood);$('moodM').classList.add('show')}
function closeMoods(){$('moodM').classList.remove('show')}
function openSounds(){nameGrid($('allS'),SOUNDS,sendSound);$('sndM').classList.add('show')}
function closeSounds(){$('sndM').classList.remove('show')}
function openMoves(){nameGrid($('allM'),MOVES,sendMove);$('movM').classList.add('show')}
function closeMoves(){$('movM').classList.remove('show')}
/* One button per expression CHANNEL. Each opens its own picker, so a face, a
   sound and a movement can be auditioned independently while the vocabulary
   is still being decided. Labels are named actions ending in "…" to signal
   that a tap opens a picker rather than firing an expression. DSGN-12. */
sheetGrid($('expF'),['Face…','Sound…','Move…'],i=>[openMoods,openSounds,openMoves][i]());
openWs();paintVals();
</script>
</body></html>
)OD";
