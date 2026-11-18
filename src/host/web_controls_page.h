#pragma once
// The TV remains the display. This page acts as an input device for Tiramisu.
static constexpr char k_controls_page[] = R"HTML(
<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tiramisu Controls</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#15100d;color:#f3dbc4;font:16px system-ui}
main{max-width:740px;margin:6vh auto;padding:24px}h1{font-size:2rem;margin:0 0 18px}
a{color:#91d4c9}input,button{font:inherit;padding:.65em;border-radius:8px;border:1px solid #8c5c3e}
input{background:#261b16;color:inherit;width:12ch}button{background:#b2754a;color:#160c05;cursor:pointer}
#pad{margin-top:18px;aspect-ratio:16/9;background:#261b16;border:1px solid #8c5c3e;
border-radius:14px;display:grid;place-items:center;text-align:center;padding:20px;touch-action:none}
#pad:focus{outline:2px solid #91d4c9}#pad[hidden]{display:none}
</style>
<main><a href="/">Tiramisu dashboard</a><h1>Keyboard and mouse</h1>
<p>Watch the Xbox screen while you play. Enter the code shown on Tiramisu’s Home or Setup screen.</p>
<form id="pair"><label for="pin">Pairing code</label> <input id="pin" inputmode="numeric"
maxlength="8" required pattern="[0-9]{8}" autocomplete="off"> <button>Pair</button></form>
<p id="status" role="status"></p>
<div id="pad" tabindex="0" hidden>Click here to capture the mouse and keyboard.<br>
Press Esc to free your mouse. You can also use this box as a trackpad.</div>
<button id="disconnect" hidden>Disconnect</button>
</main>
<script>
const pair=document.querySelector('#pair'), pin=document.querySelector('#pin');
const status=document.querySelector('#status'), pad=document.querySelector('#pad');
const disconnect=document.querySelector('#disconnect');
let token='', pending=[], busy=false, last=0, locked=false;
let mx=0,my=0,keys=new Set(),buttons=new Set(), preventClick=false;
const active=()=>!!token&&(locked||document.activeElement===pad);
function push(line){if(pending.length<128)pending.push(line);else release();}
function motion(){if(mx||my){push('m '+Math.round(mx)+' '+Math.round(my));mx=my=0;}}
function release(){pending=['r 0 0'];keys.clear();buttons.clear();mx=my=0;}
pair.onsubmit=async e=>{
  e.preventDefault();
  try {
    const res=await fetch('/api/controls/pair',{method:'POST',body:pin.value});
    if(!res.ok){status.textContent=res.status===429?'Wait a minute before trying again.':'Check the code on the Xbox.';return;}
    token=(await res.text()).trim();pending=[];last=0;pin.value='';
    pair.hidden=true;pad.hidden=false;disconnect.hidden=false;
    status.textContent='Paired. Click the box to start.';
  } catch {status.textContent='Could not reach Tiramisu.';}
};
pad.addEventListener('pointerdown',e=>{
  if(!token)return;
  if(!locked&&document.activeElement!==pad){
    pad.focus();preventClick=true;
    try{const p=pad.requestPointerLock();if(p)p.catch(()=>{});}catch{}
    e.preventDefault();return;
  }
  if(preventClick){preventClick=false;return;}
  motion();buttons.add(e.button);push('b '+e.button+' 1');e.preventDefault();
});
window.addEventListener('pointerup',e=>{
  if(!token)return;
  if(preventClick){preventClick=false;return;}
  if(!buttons.delete(e.button))return;
  motion();push('b '+e.button+' 0');e.preventDefault();
});
pad.addEventListener('pointermove',e=>{
  if(!active())return;
  if(locked){
    mx=Math.max(-2048,Math.min(2048,mx+e.movementX));
    my=Math.max(-2048,Math.min(2048,my+e.movementY));
  } else {
    const box=pad.getBoundingClientRect();
    const x=Math.max(0,Math.min(1000,Math.round(1000*(e.clientX-box.left)/box.width)));
    const y=Math.max(0,Math.min(1000,Math.round(1000*(e.clientY-box.top)/box.height)));
    if(pending.at(-1)?.startsWith('a '))pending.pop();
    push('a '+x+' '+y);
  }
});
pad.addEventListener('contextmenu',e=>e.preventDefault());
pad.addEventListener('wheel',e=>{
  if(!active()||!e.deltaY)return;
  motion();push('w '+(e.deltaY<0?120:-120)+' 0');e.preventDefault();
},{passive:false});
function vk(e){
  if(e.key==='Shift')return e.location===2?161:160;
  if(e.key==='Control')return e.location===2?163:162;
  if(e.key==='Alt')return e.location===2?165:164;
  return e.keyCode;
}
window.addEventListener('keydown',e=>{
  if(!active()||!vk(e)||(locked&&e.key==='Escape'))return;
  const k=vk(e);keys.add(k);push('k '+k+' 1');
  if(e.key.length===1&&!e.ctrlKey&&!e.altKey&&!e.metaKey)push('t '+e.key.charCodeAt(0)+' 0');
  e.preventDefault();
});
window.addEventListener('keyup',e=>{
  if(!token||!keys.delete(vk(e)))return;
  push('k '+vk(e)+' 0');e.preventDefault();
});
document.addEventListener('pointerlockchange',()=>{
  locked=document.pointerLockElement===pad;
  if(!locked){release();pad.blur();}
  status.textContent=locked?'Mouse captured. Press Esc to release it.':'Paired. Click the box to capture input.';
});
window.addEventListener('blur',()=>{if(token)release();});
document.addEventListener('visibilitychange',()=>{if(token&&document.hidden)release();});
disconnect.onclick=async()=>{
  const old=token;token='';if(locked)document.exitPointerLock();release();
  await fetch('/api/controls/leave',{method:'POST',headers:{Authorization:'Bearer '+old}}).catch(()=>{});
  pair.hidden=false;pad.hidden=true;disconnect.hidden=true;status.textContent='Disconnected.';
};
setInterval(async()=>{
  if(!token||busy)return;
  motion();
  if(!pending.length&&performance.now()-last<300)return;
  busy=true;const outgoing=pending;pending=[];
  try {
    const res=await fetch('/api/controls/input',{method:'POST',
      headers:{Authorization:'Bearer '+token},body:outgoing.join('\n')});
    if(!res.ok){
      if(res.status===401){token='';pair.hidden=false;pad.hidden=true;disconnect.hidden=true;if(locked)document.exitPointerLock();}
      status.textContent='Session lost. Pair again.';release();
    }
    last=performance.now();
  } catch {status.textContent='Connection lost. Waiting for the Xbox.';release();}
  finally{busy=false;}
},33);
</script>
</html>
)HTML";
