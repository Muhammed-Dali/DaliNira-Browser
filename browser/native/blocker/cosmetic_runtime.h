#pragma once

// One isolated-world lifecycle shared by static CSS and procedural rules.
inline constexpr auto kDaliNiraCosmeticRuntime = R"JS(
(function(){
 const key='__daliniraCosmeticRuntime';
 if(window[key]){window[key].setCss(%1);return;}
 const hiddenNodes=new WeakSet();
 function report(count,type){
  if(count>0){
   // WebKit registers this handler only in DaliNira's isolated world, so page
   // scripts cannot post fake hits. Chromium keeps the console sentinel.
   const bridge=window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.daliniraBlocker;
   if(bridge)bridge.postMessage({count:count,type:type||'cosmetic'});
   else console.log('__DALINIRA_ADBLOCK_HIT__:'+count+':'+(type||'cosmetic'));
  }
 }
 const state={css:%1,style:null,observer:null,timer:0,active:true,callbacks:[],roots:new Set(),hiddenNodes:hiddenNodes,reportBlock:report};
 window[key]=state;
 state.setCss=css=>{state.active=true;state.css=css;restore();schedule(document);};
 state.pause=()=>{state.active=false;if(state.observer)state.observer.disconnect();clearTimeout(state.timer);state.timer=0;state.roots.clear();state.callbacks=[];state.style?.remove();};
 function restore(){
  if(!state.active)return;
  if(!state.style) {state.style=document.createElement('style');state.style.id='dalinira-adblock-cosmetic';}
  if(state.style.textContent!==state.css)state.style.textContent=state.css;
  if(!state.style.isConnected)(document.head||document.documentElement)?.appendChild(state.style);
 }
 function schedule(root){
  if(!state.active)return;
  if(state.roots.size<64)state.roots.add(root||document);
  else {state.roots.clear();state.roots.add(document);}
  if(!state.timer)state.timer=setTimeout(run,180);
 }
 function checkCosmeticHits(){
  if(!state.active||!state.style)return;
  let hitCount=0;
  try{
   const sheet=state.style.sheet;
   const rules=sheet?sheet.cssRules:null;
   if(rules&&rules.length>0){
    for(let r=0;r<rules.length;r++){
     const rule=rules[r];
     const sel=rule.selectorText;
     if(!sel)continue;
     try{
      const matched=document.querySelectorAll(sel);
      for(let i=0;i<matched.length;i++){
       const el=matched[i];
       if(!hiddenNodes.has(el)){
        hiddenNodes.add(el);
        hitCount++;
       }
      }
     }catch(_){}
    }
   }
  }catch(_){}
  if(hitCount>0){
   report(hitCount,'cosmetic');
  }
 }
 function run(){
  state.timer=0;const roots=Array.from(state.roots);state.roots.clear();
  if(state.observer)state.observer.disconnect();
  try {
   restore();
   for(const callback of state.callbacks)callback(roots);
   checkCosmeticHits();
  }
  finally {
   if(state.observer){
    state.observer.observe(document,{childList:true,subtree:true,characterData:true,attributes:true,attributeFilter:['class','id','style','aria-label','badge-style-type','data-ad-preview','data-ad-comet-preview','href']});
   }
  }
 }
 state.schedule=schedule;
 state.checkCosmeticHits=checkCosmeticHits;
 state.observer=new MutationObserver(records=>{
  for(const record of records){
   if(record.target===state.style||record.target.parentNode===state.style)continue;
   schedule(record.target.nodeType===1?record.target:record.target.parentElement);
  }
 });
 state.observer.observe(document,{childList:true,subtree:true,characterData:true,attributes:true,attributeFilter:['class','id','style','aria-label','badge-style-type','data-ad-preview','data-ad-comet-preview','href']});
 for(const name of ['yt-navigate-finish','yt-page-data-updated','popstate','pageshow','hashchange'])window.addEventListener(name,()=>schedule(document),true);
 document.addEventListener('DOMContentLoaded',()=>{restore();schedule(document);},{once:true});
 restore();schedule(document);
})();
)JS";
