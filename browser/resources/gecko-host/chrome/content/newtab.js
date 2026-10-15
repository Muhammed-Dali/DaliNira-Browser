/* Trusted named-page controller; no public privileged URL command. */
'use strict';
const DngNewTab=(()=>{
 const URI='about:dalinira-newtab',PUBLIC='dalinira://newtab/',FEATURE='internal-document-v1';
 const documents=new Map(),operations=new Map(),bundles=new Map();let enabled=false,epoch=0,cacheBytes=0;
 const maxBundle=16*1024*1024,maxCache=32*1024*1024;
 const visual={theme:['plain','flow'],backgroundSource:['builtin','solid-indigo','solid-magenta','solid-teal','solid-ember','gradient-violet','gradient-aurora','gradient-ocean','gradient-sunset'],backgroundVisible:'boolean',searchVisible:'boolean',clock:'boolean',date:'boolean',shortcuts:'boolean',dim:[0,80],frequentPanelOpacity:[0,100],frequentIconOpacity:[0,100],clockStyle:['classic','digital','flip','minimal','modern-analog','neon','wall'],clockPosition:['auto','center','top-left','top-right','bottom-left','bottom-right'],clockFormat:['auto','12','24']};
 const exact=(o,keys)=>o&&typeof o==='object'&&!Array.isArray(o)&&Object.keys(o).length===keys.length&&Object.keys(o).every(k=>keys.includes(k));
 const unicode=s=>{for(let i=0;i<s.length;i++){const c=s.charCodeAt(i);if(c>=0xd800&&c<=0xdbff){const next=s.charCodeAt(++i);if(!(next>=0xdc00&&next<=0xdfff))return false;}else if(c>=0xdc00&&c<=0xdfff)return false;}return true;};
 const text=(s,max)=>typeof s==='string'&&unicode(s)&&new TextEncoder().encode(s).length<=max&&!/[\u0000-\u001f\u007f]/.test(s);
 const url=s=>{try{const u=Services.io.newURI(s);return ['http','https'].includes(u.scheme)&&!u.userPass;}catch(e){return false;}};
 const id=s=>typeof s==='string'&&/^[a-zA-Z0-9_-]{1,64}$/.test(s);
 function validVisual(key,value){const rule=visual[key];return rule==='boolean'?typeof value==='boolean':Array.isArray(rule)&&typeof rule[0]==='number'?Number.isInteger(value)&&value>=rule[0]&&value<=rule[1]:Array.isArray(rule)&&rule.includes(value);}
 function stateValid(s){return exact(s,['revision','providerId','providerLabel','placeholder','providers','items','visual','source'])&&Number.isSafeInteger(s.revision)&&s.revision>0&&text(s.providerId,180)&&text(s.providerLabel,180)&&text(s.placeholder,512)&&Array.isArray(s.providers)&&s.providers.length<=32&&s.providers.every(p=>exact(p,['id','label'])&&text(p.id,180)&&text(p.label,180))&&Array.isArray(s.items)&&s.items.length<=10&&s.items.every(p=>exact(p,['id','name','url'])&&id(p.id)&&text(p.name,720)&&text(p.url,2048)&&url(p.url))&&['frequent','bookmarks','custom'].includes(s.source)&&exact(s.visual,Object.keys(visual))&&Object.entries(s.visual).every(([k,v])=>validVisual(k,v));}
 function invalidate(tab,allowReady=false){const p=documents.get(tab.id);if(p){p.epoch=++epoch;p.authority=false;p.allowPresent=false;p.allowReady=allowReady;p.actions.clear();}}
 function bound(p,wg){return p&&!p.retired&&!p.tab.crashed&&wg&&p.wg===wg&&wg.browsingContext===p.tab.browser.browsingContext&&wg.browsingContext.top===wg.browsingContext&&wg.browsingContext.currentWindowGlobal===wg&&wg.documentURI.spec===URI&&!wg.documentPrincipal.isSystemPrincipal&&wg.documentPrincipal.isContentPrincipal&&wg.documentPrincipal.equals(p.principal);}
 function current(tab){const p=documents.get(tab.id);return p?.ready&&bound(p,tab.browser.browsingContext?.currentWindowGlobal)?p:null;}
 async function readBundle(bundleId){
  if(!/^[0-9a-f]{32}$/.test(bundleId))throw Error('invalid-bundle');
  const root=Services.env.get('DNG_NEW_TAB_ROOT');
  if(!root||!PathUtils.isAbsolute(root))throw Error('invalid-bundle-root');
  const dir=Cc['@mozilla.org/file/local;1'].createInstance(Ci.nsIFile);dir.initWithPath(root);
  const canonical=dir.clone();canonical.normalize();if(canonical.path!==root||dir.isSymlink())throw Error('invalid-bundle-root');
  const ds=await IOUtils.stat(root,{followSymlinks:false});if(ds.type!=='directory'||(ds.permissions&0o777)!==0o700)throw Error('invalid-bundle-root');
  const path=PathUtils.join(root,bundleId+'.json'),file=dir.clone();file.append(bundleId+'.json');
  if(file.isSymlink())throw Error('invalid-bundle');
  const info=await IOUtils.stat(path,{followSymlinks:false});if(info.type!=='regular'||info.size>maxBundle||(info.permissions&0o777)!==0o600)throw Error('invalid-bundle');
  const cached=bundles.get(bundleId);if(cached){if(cached.size!==info.size||cached.modified!==info.lastModified)throw Error('immutable-bundle-changed');return cached.markup;}
  const raw=await IOUtils.read(path,{maxBytes:maxBundle+1});if(raw.length!==info.size)throw Error('invalid-bundle');
  const data=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(raw));
  if(!exact(data,['version','html','sha256'])||data.version!==1||typeof data.html!=='string'||typeof data.sha256!=='string'||!/^([0-9a-f]{64})$/.test(data.sha256)||/<\s*(script|iframe|object|embed|base)\b/i.test(data.html))throw Error('invalid-bundle');
  const bytes=new TextEncoder().encode(data.html),hash=Cc['@mozilla.org/security/hash;1'].createInstance(Ci.nsICryptoHash);hash.init(hash.SHA256);hash.update(bytes,bytes.length);
  if([...hash.finish(false)].map(c=>c.charCodeAt(0).toString(16).padStart(2,'0')).join('')!==data.sha256)throw Error('invalid-bundle');
  const parsed=new DOMParser().parseFromString(data.html,'text/html');
  if(!parsed.nodePrincipal.isNullPrincipal||parsed.nodePrincipal.isSystemPrincipal||parsed.querySelector('script,iframe,object,embed,base,link,meta[http-equiv]'))throw Error('invalid-bundle');
  for(const element of parsed.querySelectorAll('*'))for(const attribute of element.attributes){
   const name=attribute.name.toLowerCase(),value=attribute.value;
   if(name.startsWith('on')||['srcdoc','action','formaction'].includes(name)||(name==='src'&&value&&!value.startsWith('data:image/'))||(name==='href'&&value&&!value.startsWith('#')))throw Error('invalid-bundle');
  }
  if(cacheBytes+bytes.length>maxCache)throw Error('bundle-cache-limit');
  cacheBytes+=bytes.length;bundles.set(bundleId,{markup:data.html,size:info.size,modified:info.lastModified});return data.html;
 }
 function report(tab){const p=current(tab);return p?{url:PUBLIC,internalPage:'newtab',internalInstance:p.instance,internalRevision:p.state.revision}:{};}
 async function command(msg){
  const response={command:msg.type,requestId:msg.requestId,tabId:msg.tabId,documentId:msg.documentId};
  let restore=null;
  try{
   if(!enabled||DngIpc.protocol!==3||msg.protocol!==3||!id(msg.requestId))throw Error('internal-feature-required');
   const schemas={load_internal_page:['page','bundleId','state'],internal_state:['instance','state'],commit_internal_action:['instance','actionId','revision','effect'],retire_internal:['instance']};
   const fields=schemas[msg.type];if(!fields||Object.keys(msg).some(k=>!['type','protocol','requestId','sessionId','commandSequence','tabId','documentId',...fields].includes(k)))throw Error('invalid-argument');
   const tab=DngTabs.tabs.get(msg.tabId);if(!tab||typeof msg.documentId!=='string'||!msg.documentId||DngTabs.documentId(tab)!==msg.documentId)throw Error('stale-document');
   if(tab.crashed)throw Error('recovery-required');
   if(msg.type==='load_internal_page'){
    if(msg.page!=='newtab'||!stateValid(msg.state))throw Error('invalid-argument');
    const browser=tab.browser,wg=browser.browsingContext.currentWindowGlobal;
    for(const [key,value] of operations)if(value.tab===tab){value.cancelled=true;operations.delete(key);}
    invalidate(tab);const oldBinding=documents.get(tab.id);if(oldBinding)restore={binding:oldBinding,epoch:oldBinding.epoch};const op={tab};operations.set(msg.requestId,op);
    let markup;try{markup=await readBundle(msg.bundleId);const available=await Promise.race([wg.getActor('DaliNiraNewTab').sendQuery('NewTab:Availability',{}),sleep(3000).then(()=>null)]);if(available?.available!==true)throw Error('actor-unavailable');}finally{if(operations.get(msg.requestId)===op)operations.delete(msg.requestId);else throw Error('cancelled');}
    if(DngTabs.tabs.get(tab.id)!==tab||tab.browser!==browser||browser.browsingContext.currentWindowGlobal!==wg||op.cancelled)throw Error('stale-document');
    if(browser.hasBeforeUnload)throw Error('beforeunload-navigation-blocked');
    const old=documents.get(tab.id);const bytes=new TextEncoder().encode(markup).length;

    documents.set(tab.id,{tab,markup,bytes,state:msg.state,epoch:++epoch,allowReady:true,ready:false,actions:new Map(),oldWG:wg,retired:false});
    browser.loadURI(Services.io.newURI(URI),systemLoadOptions());
    const loading=documents.get(tab.id);setTimeout(()=>{if(documents.get(tab.id)===loading&&!loading.ready&&!tab.crashed&&DngTabs.tabs.has(tab.id))ipcTabSend(tab,'load-error',{error:'internal-initialization-failed',url:URI});},5000);
   }else{
    const p=current(tab);if(!p||msg.instance!==p.instance)throw Error('stale-document');
    if(msg.type==='retire_internal'){p.retired=true;invalidate(tab);}
    else if(msg.type==='internal_state'){
     if(!stateValid(msg.state)||msg.state.revision<=p.state.revision)throw Error('stale-state');
     p.state=msg.state;p.actions.clear();p.wg.getActor('DaliNiraNewTab').sendAsyncMessage('NewTab:State',{instance:p.instance,state:p.state});ipcReportState(pageState(tab.browser),tab);
    }else{
     response.instance=msg.instance;response.actionId=msg.actionId;response.revision=msg.revision;
     const action=p.actions.get(msg.actionId);p.actions.delete(msg.actionId);
     if(!p.authority||!action||msg.revision!==p.state.revision||tab.id!==DngTabs.activeId||Date.now()-action.time>3000)throw Error('stale-action');
     if(exact(msg.effect,['kind','url'])&&msg.effect.kind==='navigate'&&['search','speed-dial'].includes(action.data.kind)){
      const u=Services.io.newURI(msg.effect.url);if(!['http','https'].includes(u.scheme)||u.userPass)throw Error('url-rejected');
      if(tab.browser.hasBeforeUnload)throw Error('beforeunload-navigation-blocked');
      invalidate(tab);tab.lastUrl=u.spec;tab.browser.loadURI(u,systemLoadOptions());
     }else if(!(exact(msg.effect,['kind'])&&msg.effect.kind==='preference'&&['visual','provider','source','shortcut-add','shortcut-edit','shortcut-remove'].includes(action.data.kind)))throw Error('invalid-effect');
    }
   }
   DngIpc.send('ack',response);
  }catch(e){if(restore&&documents.get(restore.binding.tab.id)===restore.binding&&restore.binding.epoch===restore.epoch&&restore.binding.ready&&bound(restore.binding,restore.binding.tab.browser.browsingContext.currentWindowGlobal))restore.binding.authority=true;DngIpc.send('error',{...response,code:String(e.message||'internal-failure').slice(0,80)});}
 }
 function receive(actor,msg){
  const wg=actor.manager,bc=wg.browsingContext,tab=DngTabs.tabForBrowser(bc.top.embedderElement),p=tab&&documents.get(tab.id);
  if(!enabled||!p||p.retired||tab.crashed||bc!==bc.top||bc!==tab.browser.browsingContext||bc.currentWindowGlobal!==wg||wg.documentURI.spec!==URI||wg.documentPrincipal.isSystemPrincipal||!wg.documentPrincipal.isContentPrincipal)return {ok:false};
  if(msg.name==='NewTab:Ready'){
   if(!p.allowReady||p.oldWG===wg||!exact(msg.data,[]))return {ok:false};
   const expected=Services.scriptSecurityManager.createContentPrincipal(Services.io.newURI(URI),{});if(!wg.documentPrincipal.equals(expected))return {ok:false};
   p.allowReady=false;p.allowPresent=true;p.lastProposal=0;p.wg=wg;p.principal=wg.documentPrincipal;p.instance=Services.uuid.generateUUID().toString().replace(/[{}-]/g,'');p.actions.clear();
   return {ok:true,markup:p.markup,state:p.state,instance:p.instance,draft:p.draft||'',activation:p.activation||0};
  }
  if(!bound(p,wg))return {ok:false};
  if(msg.name==='NewTab:Draft'&&p.ready&&p.authority&&tab.id===DngTabs.activeId&&exact(msg.data,['draft'])&&text(msg.data.draft,4096)){p.draft=msg.data.draft;return {ok:true};}
  if(msg.name==='NewTab:Presented'&&p.allowPresent&&exact(msg.data,['instance'])&&msg.data.instance===p.instance){p.allowPresent=false;p.ready=true;p.authority=true;tab.lastUrl=URI;ipcReportState(pageState(tab.browser),tab);return {ok:true};}
  if(msg.name!=='NewTab:Action'||!p.ready||!p.authority||tab.id!==DngTabs.activeId||!exact(msg.data,['instance','revision','action','activation','sequence'])||msg.data.instance!==p.instance||msg.data.activation!==(p.activation||0)||msg.data.revision!==p.state.revision||!Number.isSafeInteger(msg.data.sequence)||msg.data.sequence<=p.lastProposal||Date.now()-(p.lastAction||0)<80)return {ok:false};
  const a=msg.data.action;let valid=false;
  if(exact(a,['kind','query'])&&a.kind==='search')valid=text(a.query,4096)&&!!a.query.trim();
  if(exact(a,['kind','itemId'])&&a.kind==='speed-dial')valid=id(a.itemId)&&p.state.items.some(i=>i.id===a.itemId);
  if(exact(a,['kind','value'])&&a.kind==='provider')valid=p.state.providers.some(i=>i.id===a.value);
  if(exact(a,['kind','key','value'])&&a.kind==='visual')valid=validVisual(a.key,a.value);
  if(exact(a,['kind','value'])&&a.kind==='source')valid=['frequent','bookmarks','custom'].includes(a.value);
  if((exact(a,['kind','name','url'])&&a.kind==='shortcut-add')||(exact(a,['kind','itemId','name','url'])&&a.kind==='shortcut-edit')){
   valid=p.state.source==='custom'&&text(a.name,720)&&!!a.name.trim()&&text(a.url,2048)&&url(a.url)&&(a.kind==='shortcut-add'||p.state.items.some(i=>i.id===a.itemId));
  }
  if(exact(a,['kind','itemId'])&&a.kind==='shortcut-remove')valid=p.state.source==='custom'&&p.state.items.some(i=>i.id===a.itemId);
  if(!valid)return {ok:false};
  p.lastProposal=msg.data.sequence;p.lastAction=Date.now();if(p.actions.size>=8)return {ok:false};
  const actionId=Services.uuid.generateUUID().toString().replace(/[{}-]/g,'');p.actions.set(actionId,{data:a,time:Date.now()});
  ipcTabSend(tab,'internal-action',{documentId:DngTabs.documentId(tab),instance:p.instance,revision:p.state.revision,actionId,action:a});return {ok:true};
 }
 function install(){
  ChromeUtils.importESModule('chrome://dng/content/NewTabAbout.sys.mjs').installNewTabAbout();
  Services.ppmm.loadProcessScript('chrome://dng/content/newtab-bootstrap.js',true);
  ChromeUtils.registerWindowActor('DaliNiraNewTab',{safeForUntrustedWebProcess:true,remoteTypes:['web'],allFrames:false,includeChrome:false,messageManagerGroups:['browsers'],parent:{esModuleURI:'chrome://dng/content/DaliNiraNewTabParent.sys.mjs'},child:{esModuleURI:'chrome://dng/content/DaliNiraNewTabChild.sys.mjs',events:{DOMContentLoaded:{capture:true},pageshow:{capture:true},click:{capture:true},keydown:{capture:true},input:{capture:true},change:{capture:true}}}});
 }
 return {install,receive,command,report,stateValid,feature:FEATURE,
  async testOperation(tab,operation){
   const p=current(tab);if(Services.env.get('DNG_TEST_INPUT')!=='1'||!p||!['inspect','render','security','focus-search','focus-first-site','focus-theme','focus-customize','focus-close-customize','focus-add','focus-shortcut-name','focus-shortcut-url','focus-shortcut-cancel','focus-last-more','focus-shortcut-edit','focus-shortcut-remove'].includes(operation))throw Error('invalid-test-operation');
   if(operation==='security')return {runtime:await runtimeFacts(),graphics:gfxFacts()};
   if(operation==='render'){
    const bitmap=await p.wg.drawSnapshot(null,1,'white');const canvas=document.createElementNS(XHTML_NS,'canvas');canvas.width=bitmap.width;canvas.height=bitmap.height;canvas.getContext('2d').drawImage(bitmap,0,0);const blob=await new Promise(resolve=>canvas.toBlob(resolve,'image/png'));await IOUtils.write(PathUtils.join(Services.env.get('DNG_NEW_TAB_ROOT'),'newtab-render.png'),new Uint8Array(await blob.arrayBuffer()));return {width:bitmap.width,height:bitmap.height};
   }
   return p.wg.getActor('DaliNiraNewTab').sendQuery('NewTab:Test',{operation});
  },negotiate(features){enabled=features?.includes(FEATURE)===true;if(enabled){try{install();}catch(e){enabled=false;dump('NewTab installation failed: '+e+'\n');}}},
  activation(){for(const p of documents.values()){p.actions.clear();p.epoch=++epoch;p.activation=(p.activation||0)+1;if(!Number.isSafeInteger(p.activation)){p.retired=true;continue;}if(p.ready&&bound(p,p.wg))p.wg.getActor('DaliNiraNewTab').sendAsyncMessage('NewTab:Activation',{instance:p.instance,activation:p.activation});}},
  intent(tab,type){for(const [key,value] of operations)if(value.tab===tab){value.cancelled=true;operations.delete(key);}invalidate(tab,['back','forward','reload','recover_tab'].includes(type));},
  cancel(requestId){const op=operations.get(requestId);if(op){op.cancelled=true;operations.delete(requestId);epoch++;}},
  retire(tab){const p=documents.get(tab.id);if(p){documents.delete(tab.id);}epoch++;},
  resume(tab){const p=current(tab);if(p)p.authority=true;},
  revokeView(tab){const p=documents.get(tab.id);if(p){p.retired=true;invalidate(tab);}}
 };
})();
window.DngNewTab=DngNewTab;
