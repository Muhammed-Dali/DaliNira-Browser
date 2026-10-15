export class DaliNiraNewTabChild extends JSWindowActorChild {
 async handleEvent(e){
  const d=this.document;
  if(e.type==='DOMContentLoaded'||e.type==='pageshow'){
   if(e.originalTarget!==d||d.documentURI!=='about:dalinira-newtab')return;
   const payload=await this.sendQuery('NewTab:Ready',{});
   if(!payload?.ok||this.document!==d||d.defaultView.document!==d)return;
   const parsed=new d.defaultView.DOMParser().parseFromString(payload.markup,'text/html');
   // This transport contains markup only; no executable or frame content.
   if(parsed.querySelector('script,iframe,object,embed,base,meta[http-equiv]'))throw Error('invalid markup');
   for(const el of parsed.querySelectorAll('*'))for(const attr of el.attributes)if(attr.name.toLowerCase().startsWith('on'))throw Error('event attribute');
   if(!(e.type==='pageshow'&&e.persisted))d.replaceChild(d.importNode(parsed.documentElement,true),d.documentElement);
   this.editItem=null;this.sequence=0;this.activation=payload.activation;this.instance=payload.instance;this.revision=payload.state.revision;
   if(typeof payload.draft==='string')d.getElementById('query').value=payload.draft;
   this.state(payload.state);
   await this.sendQuery('NewTab:Presented',{instance:this.instance});return;
  }
  if(!this.instance||!e.isTrusted||e.originalTarget?.ownerDocument!==d||d.defaultView.document!==d||
    this.browsingContext!==this.browsingContext.top||!d.defaultView.windowUtils.isHandlingUserInput)return;
  const t=e.originalTarget,b=t.closest?.('button,[data-dng-item]');let action=null;
  if(e.type==='keydown'&&e.key==='Enter'&&t.id==='query'){e.preventDefault();action={kind:'search',query:t.value};}
  const shortcut=()=>{const name=d.getElementById('shortcut-name').value,url=d.getElementById('shortcut-url').value.includes('://')?d.getElementById('shortcut-url').value:'https://'+d.getElementById('shortcut-url').value;return this.editItem?{kind:'shortcut-edit',itemId:this.editItem,name,url}:{kind:'shortcut-add',name,url};};
  if(e.type==='keydown'&&e.key==='Enter'&&['shortcut-name','shortcut-url'].includes(t.id)&&!d.getElementById('shortcut-save').disabled){e.preventDefault();action=shortcut();}
  if(e.type==='click'&&e.button===0){
   if(b?.dataset.dngMore)this.editItem=b.dataset.dngMore;
   if(b?.dataset.dngAdd)this.editItem=null;
   if(b?.id==='shortcut-save'&&!b.disabled)action=shortcut();
   if(b?.id==='shortcut-menu-remove'&&this.editItem)action={kind:'shortcut-remove',itemId:this.editItem};
   if(t.name==='top-sites-source')action={kind:'source',value:t.value};
   if(b?.dataset.dngItem)action={kind:'speed-dial',itemId:b.dataset.dngItem};
   else if(b?.dataset.engine)action={kind:'provider',value:b.dataset.engine};
   else if(b?.dataset.theme)action={kind:'visual',key:'theme',value:b.dataset.theme};
   else if(b?.dataset.background&&b.dataset.background!=='custom')action={kind:'visual',key:'backgroundSource',value:b.dataset.background};
   else if(b?.dataset.clockStyle)action={kind:'visual',key:'clockStyle',value:b.dataset.clockStyle};
   else if(b?.dataset.clockPosition)action={kind:'visual',key:'clockPosition',value:b.dataset.clockPosition};
   const map={'background-toggle':'backgroundVisible','search-toggle':'searchVisible','clock-toggle':'clock','date-toggle':'date','shortcuts-toggle':'shortcuts'};
   if(map[t.id])action={kind:'visual',key:map[t.id],value:t.checked};
  }
  if(e.type==='change'&&t.id==='clock-format')action={kind:'visual',key:'clockFormat',value:t.value};
  if(e.type==='input'&&t.id==='query')this.sendAsyncMessage('NewTab:Draft',{draft:t.value});
  if(e.type==='input'){
   const map={'dim':'dim','frequent-panel-opacity':'frequentPanelOpacity','frequent-icon-opacity':'frequentIconOpacity'};
   if(map[t.id])action={kind:'visual',key:map[t.id],value:Number(t.value)};
  }
  if(action){
   e.preventDefault();if(!Number.isSafeInteger(++this.sequence))return;const proposal={instance:this.instance,revision:this.revision,activation:this.activation,sequence:this.sequence,action};
   if(e.type==='input'&&action.kind==='visual'){
    if(this.sliderTimer)d.defaultView.clearTimeout(this.sliderTimer);
    this.sliderTimer=d.defaultView.setTimeout(()=>{try{if(this.document===d&&d.defaultView.document===d&&this.revision===proposal.revision&&this.activation===proposal.activation)this.sendQuery('NewTab:Action',proposal);}catch(e){}},100);
   }else this.sendQuery('NewTab:Action',proposal);
  }
 }
 state(state){const d=this.document;this.revision=state.revision;d.dispatchEvent(new d.defaultView.CustomEvent('DngNewTabState',{detail:Cu.cloneInto(state,d.defaultView)}));}
 receiveMessage(msg){
  const d=this.document;
  if(msg.name==='NewTab:Availability')return {available:true};
  if(msg.name==='NewTab:Test'&&Services.env.get('DNG_TEST_INPUT')==='1'){
   if(msg.data.operation==='inspect')return {uri:d.documentURI,title:d.title,query:!!d.getElementById('query'),queryValue:d.getElementById('query')?.value,items:d.querySelectorAll('[data-dng-item]').length,images:d.querySelectorAll('img[src^="data:"]').length,system:d.nodePrincipal.isSystemPrincipal,content:d.nodePrincipal.isContentPrincipal,security:d.getElementById('dng-security-fixture')?.textContent||'',Services:typeof d.defaultView.wrappedJSObject.Services};
   const target=msg.data.operation==='focus-last-more'?[...d.querySelectorAll('[data-dng-more]')].at(-1):msg.data.operation==='focus-shortcut-edit'?d.getElementById('shortcut-menu-edit'):msg.data.operation==='focus-shortcut-remove'?d.getElementById('shortcut-menu-remove'):msg.data.operation==='focus-search'?d.getElementById('query'):msg.data.operation==='focus-first-site'?d.querySelector('[data-dng-item]'):msg.data.operation==='focus-theme'?d.querySelector('[data-theme="flow"]'):msg.data.operation==='focus-customize'?d.getElementById('customize'):msg.data.operation==='focus-close-customize'?d.getElementById('customization-close'):msg.data.operation==='focus-add'?d.querySelector('[data-dng-add]'):msg.data.operation==='focus-shortcut-name'?d.getElementById('shortcut-name'):msg.data.operation==='focus-shortcut-url'?d.getElementById('shortcut-url'):msg.data.operation==='focus-shortcut-cancel'?d.getElementById('shortcut-cancel'):null;
   if(target){target.focus();return {focused:d.activeElement===target};}return {focused:false};
  }
  if(msg.name==='NewTab:Activation'&&this.instance===msg.data.instance){this.activation=msg.data.activation;return;}
  if(msg.name==='NewTab:State'&&this.instance===msg.data.instance&&d.documentURI==='about:dalinira-newtab'&&d.defaultView.document===d)this.state(msg.data.state);
 }
}
