#pragma once

// DaliNira's element picker, run in the blocker's isolated JavaScript world
// (the page's scripts can neither read nor call it). Hover highlighting,
// selection, selector candidates and the preview all stay in the page; native
// code only polls takeEvent() for the user's decisions and answers with
// saved()/close(). The overlay lives in a closed shadow root and only ever
// displays text (textContent), never markup. The selectors it proposes are
// plain CSS: the native side turns the chosen one into "host##selector" and
// validates it again before anything is saved.
//
// __DALINIRA_PICKER_CONFIG__ is replaced by the configuration (JSON, built natively).
inline constexpr auto kDaliNiraElementPickerRuntime = R"JS(
(function(config){
'use strict';
const KEY='__daliniraPicker';
if(window[KEY])window[KEY].close('restart');
if(!document.documentElement)return;
const S=config.strings||{};
const MAX_CANDIDATES=Math.max(1,Math.min(8,config.maxCandidates|0||6));
const MAX_LENGTH=Math.max(32,Math.min(512,config.maxSelectorLength|0||256));
const BROAD=Math.max(10,config.broadMatches|0||100);
const MAX_DEPTH=6;
let mode='picking';        // picking | selected | saved | closed
let hovered=null,selected=null,candidates=[],current=-1,previewing=true;
const events=[];
let previewStyle=null,appliedStyle=null,autoClose=0;

const hostElement=document.createElement('dalinira-element-picker');
const root=hostElement.attachShadow({mode:'closed'});
const style=document.createElement('style');
style.textContent=
 ':host{all:initial}'+
 '.box{position:fixed;pointer-events:none;z-index:2147483646;border:2px solid #4ec9ff;background:rgba(78,201,255,.18);border-radius:3px;box-sizing:border-box;display:none}'+
 '.match{position:fixed;pointer-events:none;z-index:2147483645;border:1px dashed #f2b35c;box-sizing:border-box}'+
 '.panel{position:fixed;right:16px;bottom:16px;z-index:2147483647;width:360px;max-width:calc(100vw - 32px);max-height:60vh;overflow:auto;'+
 'background:#181f28;color:#e4ebf5;border:1px solid #283442;border-radius:10px;padding:12px;font:13px/1.4 system-ui,sans-serif;'+
 'box-shadow:0 8px 28px rgba(0,0,0,.45);direction:ltr;text-align:start}'+
 '.title{font-weight:700;font-size:14px;margin-bottom:6px}.hint{color:#8c9ba8;font-size:12px;margin-bottom:8px}'+
 '.cand{display:block;width:100%;box-sizing:border-box;text-align:start;margin:4px 0;padding:6px 8px;border-radius:7px;cursor:pointer;'+
 'background:#121720;color:#e4ebf5;border:1px solid #24303f;font:12px/1.3 ui-monospace,monospace;word-break:break-all}'+
 '.cand.on{border-color:#4ec9ff;background:#10243a}.cand .n{display:block;color:#8c9ba8;font:11px system-ui,sans-serif;margin-top:2px}'+
 '.cand.broad .n{color:#f2b35c}.row{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}'+
 'button.act{flex:1 1 auto;padding:6px 10px;border-radius:7px;border:1px solid #2e598a;background:#1f3b5c;color:#e4ebf5;cursor:pointer;font:12px system-ui,sans-serif}'+
 'button.primary{background:#1b6fb0;border-color:#4ec9ff}button.act:disabled{opacity:.45;cursor:default}'+
 '.status{margin-top:8px;font-size:12px;color:#8c9ba8}.status.warn{color:#f2b35c}';
root.appendChild(style);
const box=document.createElement('div');box.className='box';root.appendChild(box);
const matchLayer=document.createElement('div');root.appendChild(matchLayer);
const panel=document.createElement('div');panel.className='panel';root.appendChild(panel);

function text(tag,cls,value){const e=document.createElement(tag);if(cls)e.className=cls;e.textContent=value==null?'':String(value);return e;}
function button(label,cls,action){const b=text('button','act'+(cls?' '+cls:''),label);b.type='button';
 b.addEventListener('click',e=>{e.preventDefault();e.stopPropagation();action();});return b;}
function format(template,value){return String(template||'').replace('%1',String(value));}

// ---- selection ----------------------------------------------------------
function isOurs(node){return node===hostElement;}
function selectable(el){return el&&el.nodeType===1&&!isOurs(el)&&el!==document.documentElement&&el!==document.body&&el!==document.head;}
function elementAt(x,y){
 const list=document.elementsFromPoint(x,y)||[];
 for(const el of list)if(selectable(el))return el;
 return null;
}
function place(node,el){
 const r=el.getBoundingClientRect();
 node.style.left=r.left+'px';node.style.top=r.top+'px';node.style.width=Math.max(0,r.width)+'px';node.style.height=Math.max(0,r.height)+'px';
}
function hover(el){
 if(mode!=='picking')return;
 hovered=selectable(el)?el:null;
 if(!hovered){box.style.display='none';return;}
 box.style.display='block';place(box,hovered);
}

// ---- candidates ---------------------------------------------------------
// Framework-generated names change between page loads; prefer the rest.
function stable(token){
 if(!token||token.length<2||token.length>40)return false;
 if(/^[0-9]/.test(token)||/\d{4,}/.test(token)||/[0-9a-f]{8,}/i.test(token))return false;
 if(/^(css|sc|jsx|emotion|svelte|styled|tw)-/i.test(token)||/__[A-Za-z0-9_-]{5,}$/.test(token))return false;
 if(/[A-Z]/.test(token)&&/[a-z]/.test(token)&&/\d/.test(token)&&token.length>=6)return false;
 return true;
}
const allCount=()=>document.getElementsByTagName('*').length;
// Match count, or null when the selector is invalid, targets the document's
// structure, or covers most of the page.
function measure(selector){
 let list;
 try{list=document.querySelectorAll(selector);}catch(_){return null;}
 const total=allCount();
 let count=0;
 for(const el of list){
  if(el===document.documentElement||el===document.body||el===document.head||isOurs(el))return null;
  if(count<50&&el.getElementsByTagName('*').length>total*0.5)return null;
  if(++count>2000)break;
 }
 return {count,list};
}
const ESCAPE=window.CSS&&CSS.escape?CSS.escape:(s=>String(s).replace(/[^a-zA-Z0-9_-]/g,c=>'\\'+c));
function attributeValue(v){return '"'+String(v).replace(/["\\\n\r]/g,c=>c==='\n'||c==='\r'?' ':'\\'+c)+'"';}
function classesOf(el){return Array.from(el.classList||[]).filter(stable).slice(0,3);}
function anchorOf(el){
 for(let p=el.parentElement,depth=0;p&&depth<MAX_DEPTH;p=p.parentElement,depth++){
  if(p===document.body||p===document.documentElement)break;
  if(p.id&&stable(p.id))return p;
 }
 return null;
}
function nthPath(el){
 const parts=[];
 let e=el;
 for(let depth=0;e&&depth<MAX_DEPTH;e=e.parentElement,depth++){
  if(e===document.body||e===document.documentElement)break;
  if(e!==el&&e.id&&stable(e.id)){parts.unshift('#'+ESCAPE(e.id));return parts.join(' > ');}
  let n=1;for(let s=e.previousElementSibling;s;s=s.previousElementSibling)if(s.localName===e.localName)n++;
  parts.unshift(e.localName+':nth-of-type('+n+')');
 }
 // Anchored at body only when the walk actually reached it.
 return (e===document.body?'body > ':'')+parts.join(' > ');
}
function generate(el){
 const out=[];
 const add=(selector,kind)=>{
  if(!selector||selector.length>MAX_LENGTH||out.some(c=>c.selector===selector))return;
  const m=measure(selector);
  if(!m||!m.count)return;
  if(!Array.from(m.list).includes(el))return;
  out.push({selector,kind,count:m.count,broad:m.count>BROAD});
 };
 const tag=el.localName;
 if(el.id&&stable(el.id))add('#'+ESCAPE(el.id),'id');
 const cls=classesOf(el);
 if(cls.length){
  const joined=cls.map(c=>'.'+ESCAPE(c)).join('');
  add(tag+joined,'class');add(joined,'class');
  if(cls.length>1)add(tag+'.'+ESCAPE(cls[0]),'class');
 }
 for(const name of ['data-ad','data-ad-slot','data-ad-unit','data-testid','data-test','data-qa','aria-label','role','name','title']){
  const v=el.getAttribute&&el.getAttribute(name);
  if(v==null||v.length>60||(name.startsWith('data-')&&v&&!stable(v.replace(/[\s/:.]/g,'-'))))continue;
  add(tag+'['+name+'='+attributeValue(v)+']','attribute');
 }
 const anchor=anchorOf(el);
 if(anchor){
  const anchorSel='#'+ESCAPE(anchor.id);
  if(cls.length)add(anchorSel+' '+tag+'.'+cls.map(ESCAPE).join('.'),'context');
  add(anchorSel+' '+tag,'context');
 }
 add(nthPath(el),'position');
 // Specific first; broad ones last (they cannot be saved).
 out.sort((a,b)=>(a.broad-b.broad));
 return out.slice(0,MAX_CANDIDATES);
}

// ---- preview ------------------------------------------------------------
function clearPreview(){if(previewStyle){previewStyle.remove();previewStyle=null;}matchLayer.textContent='';}
function applyPreview(){
 clearPreview();
 const c=candidates[current];
 if(!c)return;
 if(previewing&&!c.broad){
  previewStyle=document.createElement('style');
  previewStyle.textContent=c.selector+'{display:none!important}';
  (document.head||document.documentElement).appendChild(previewStyle);
 }else{
  const m=measure(c.selector);
  if(m)Array.from(m.list).slice(0,50).forEach(el=>{const d=document.createElement('div');d.className='match';place(d,el);matchLayer.appendChild(d);});
 }
}

// ---- panel --------------------------------------------------------------
function render(){
 panel.textContent='';
 if(mode==='picking'){
  panel.appendChild(text('div','title',S.title));
  panel.appendChild(text('div','hint',S.hint));
  const row=text('div','row');row.appendChild(button(S.cancel,'',()=>cancel()));panel.appendChild(row);
  return;
 }
 if(mode==='saved'||mode==='failed'){
  panel.appendChild(text('div','title',mode==='saved'?S.saved:S.saveFailed));
  if(mode==='saved'&&candidates[current])panel.appendChild(text('div','hint',candidates[current].selector));
  const row=text('div','row');
  if(mode==='saved')row.appendChild(button(S.undo,'',()=>undo()));
  row.appendChild(button(S.done,'primary',()=>close('done')));
  panel.appendChild(row);return;
 }
 panel.appendChild(text('div','title',S.title));
 if(!candidates.length){
  panel.appendChild(text('div','status warn',S.noCandidate));
 }
 candidates.forEach((c,i)=>{
  const item=text('button','cand'+(i===current?' on':'')+(c.broad?' broad':''),c.selector);
  item.type='button';
  item.appendChild(text('span','n',format(S.matches,c.count)+(c.broad?' — '+S.tooBroad:'')));
  item.addEventListener('click',e=>{e.preventDefault();e.stopPropagation();choose(i);});
  panel.appendChild(item);
 });
 const row=text('div','row');
 const toggle=button(previewing?S.previewOff:S.previewOn,'',()=>setPreview(!previewing));
 row.appendChild(toggle);
 row.appendChild(button(S.parent,'',()=>parent()));
 row.appendChild(button(S.repick,'',()=>repick()));
 panel.appendChild(row);
 const row2=text('div','row');
 const create=button(S.create,'primary',()=>confirm());
 create.disabled=!candidates[current]||candidates[current].broad;
 row2.appendChild(create);row2.appendChild(button(S.cancel,'',()=>cancel()));
 panel.appendChild(row2);
}

// ---- state machine (also the native test entry points) -------------------
function select(el){
 if(!selectable(el)||mode==='closed'||mode==='saved')return false;
 selected=el;mode='selected';box.style.display='block';place(box,el);
 candidates=generate(el);
 current=candidates.findIndex(c=>!c.broad);
 if(current<0&&candidates.length)current=0;
 applyPreview();render();return true;
}
function choose(i){if(mode!=='selected'||!candidates[i])return false;current=i;applyPreview();render();return true;}
function setPreview(on){previewing=!!on;if(mode==='selected'){applyPreview();render();}return previewing;}
function parent(){if(mode!=='selected'||!selected)return false;const p=selected.parentElement;return selectable(p)?select(p):false;}
function repick(){if(mode==='saved'||mode==='closed')return false;clearPreview();selected=null;candidates=[];current=-1;mode='picking';box.style.display='none';render();return true;}
function confirm(){
 const c=candidates[current];
 if(mode!=='selected'||!c||c.broad)return false;
 if(!measure(c.selector))return false;  // the page changed under the selector
 events.push({type:'save',selector:c.selector,count:c.count});
 return true;
}
function undo(){if(mode!=='saved')return false;if(appliedStyle){appliedStyle.remove();appliedStyle=null;}events.push({type:'undo'});return true;}
function cancel(){events.push({type:'cancel'});close('cancel');}

// ---- input --------------------------------------------------------------
function inOverlay(e){return e.composedPath&&e.composedPath().includes(hostElement);}
// Real pointer events are hit-tested (so transparent overlays do not hide
// what is under them); synthetic ones name their target.
function targetOf(e){return e.isTrusted?(elementAt(e.clientX,e.clientY)||e.target):e.target;}
function onMove(e){if(mode==='picking'&&!inOverlay(e))hover(targetOf(e));}
// While the picker is open the page gets no clicks, presses or submits:
// links do not navigate and buttons do not act.
function swallow(e){
 if(mode==='closed'||inOverlay(e))return;
 e.preventDefault();e.stopImmediatePropagation();
 if(e.type==='click'&&mode==='picking')select(targetOf(e));
}
function onKey(e){
 if(mode==='closed'||inOverlay(e))return;
 e.preventDefault();e.stopImmediatePropagation();
 if(e.type==='keydown'&&e.key==='Escape')cancel();
}
function onViewport(){if(mode==='selected'&&selected)place(box,selected);else if(hovered)place(box,hovered);}
const swallowed=['click','dblclick','auxclick','mousedown','mouseup','pointerdown','pointerup','contextmenu','submit','touchstart','touchend','selectstart','dragstart'];
window.addEventListener('mousemove',onMove,true);
window.addEventListener('mouseover',onMove,true);
for(const type of swallowed)window.addEventListener(type,swallow,true);
window.addEventListener('keydown',onKey,true);
window.addEventListener('keyup',onKey,true);
window.addEventListener('scroll',onViewport,true);
window.addEventListener('resize',onViewport,true);
function onPageHide(){close('navigation');}
window.addEventListener('pagehide',onPageHide,true);

// Everything the picker added goes: overlay, preview, listeners, and the API
// (native polling then finds no picker and ends its session).
function close(reason){
 if(mode==='closed')return;
 mode='closed';
 closeReason=String(reason||'');
 clearTimeout(autoClose);
 clearPreview();
 window.removeEventListener('mousemove',onMove,true);
 window.removeEventListener('mouseover',onMove,true);
 for(const type of swallowed)window.removeEventListener(type,swallow,true);
 window.removeEventListener('keydown',onKey,true);
 window.removeEventListener('keyup',onKey,true);
 window.removeEventListener('scroll',onViewport,true);
 window.removeEventListener('resize',onViewport,true);
 window.removeEventListener('pagehide',onPageHide,true);
 hostElement.remove();
 hovered=selected=null;candidates=[];
 if(window[KEY]===api)delete window[KEY];
}
let closeReason='';

const api={
 // Native polling: the oldest pending decision as JSON, or ''.
 takeEvent(){const e=events.shift();return e?JSON.stringify(e):'';},
 saved(ok){if(mode==='closed')return;clearPreview();
  if(ok&&candidates[current]&&!candidates[current].broad){
   appliedStyle=document.createElement('style');
   appliedStyle.textContent=candidates[current].selector+'{display:none!important}';
   (document.head||document.documentElement).appendChild(appliedStyle);
  }
  mode=ok?'saved':'failed';box.style.display='none';render();
  clearTimeout(autoClose);autoClose=setTimeout(()=>close('done'),ok?8000:4000);},
 close,
 // Native test entry points (reachable only from DaliNira's world).
 state(){return JSON.stringify({mode,hovered:!!hovered,hoveredId:hovered&&hovered.id||'',selectedId:selected&&selected.id||'',
  current,previewing,candidates:candidates.map(c=>({selector:c.selector,kind:c.kind,count:c.count,broad:c.broad})),
  overlayConnected:hostElement.isConnected,previewActive:!!previewStyle,closeReason});},
 hover,select,choose,setPreview,parent,repick,confirm,undo,cancel,generate:el=>JSON.stringify(generate(el)),measure:s=>{const m=measure(s);return m?m.count:-1;}
};
window[KEY]=api;
(document.body||document.documentElement).appendChild(hostElement);
render();
})(__DALINIRA_PICKER_CONFIG__);
)JS";
