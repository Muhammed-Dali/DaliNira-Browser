/* Fixed, content-only presentation code. It has no native API or request bridge. */
'use strict';
(() => {
 const $=id=>document.getElementById(id.startsWith('#')?id.slice(1):id);
 let state=null,editing=null,pendingShortcut=null;const icons=new Map();
 function tick(){if(!state||!$('clock'))return;const p=state.visual;const date=new Date();const browserLocale=document.documentElement.lang||(navigator.languages&&navigator.languages[0])||navigator.language||'';const locale=/^(c|posix)$/i.test(browserLocale)?undefined:browserLocale.replace('_','-');const options={hour:'numeric',minute:'2-digit'};if(p.clockFormat==='12')options.hour12=true;else if(p.clockFormat==='24')options.hour12=false;let timeStr='';try{timeStr=date.toLocaleTimeString(locale,options);$('#clock').textContent=timeStr;$('#date').textContent=date.toLocaleDateString(locale,{weekday:'long',day:'numeric',month:'long'})}catch(_){timeStr=date.toLocaleTimeString(undefined,options);$('#clock').textContent=timeStr;$('#date').textContent=date.toLocaleDateString(undefined,{weekday:'long',day:'numeric',month:'long'})}document.querySelectorAll('.preview-time').forEach(el=>el.textContent=timeStr);const seconds=date.getSeconds(),minutes=date.getMinutes()+seconds/60,hours=(date.getHours()%12)+minutes/60;const sDeg=seconds*6,mDeg=minutes*6,hDeg=hours*30;document.querySelectorAll('.clock-hand.second, .mini-second, .modern-second').forEach(el=>el.style.transform='translateX(-50%) rotate('+sDeg+'deg)');document.querySelectorAll('.clock-hand.minute, .mini-minute, .modern-minute').forEach(el=>el.style.transform='translateX(-50%) rotate('+mDeg+'deg)');document.querySelectorAll('.clock-hand.hour, .mini-hour, .modern-hour').forEach(el=>el.style.transform='translateX(-50%) rotate('+hDeg+'deg)')}
 function render(s){
  state=s;const v=s.visual;
  if(pendingShortcut&&s.items.some(item=>item.name===pendingShortcut.name&&new URL(item.url).href===pendingShortcut.url)){pendingShortcut=null;$('shortcut-overlay').hidden=true;}
  $('query').placeholder=s.placeholder;
  $('engine-current').title=s.providerLabel;
  $('engine-current-icon').alt=s.providerLabel;
  const menu=$('engine-menu');if(!icons.size)for(const b of menu.querySelectorAll('[data-engine]')){const img=b.querySelector('img');if(img?.src.startsWith('data:'))icons.set(b.dataset.engine,img.src);}menu.replaceChildren();
  for(const p of s.providers){const b=document.createElement('button');b.type='button';b.className='engine-option';b.dataset.engine=p.id;const label=document.createElement('span');label.className='engine-option-label';label.textContent=p.label;const logo=document.createElement('span');logo.className='engine-logo';const image=document.createElement('img');image.src=icons.get(p.id)||icons.get('DuckDuckGo')||'';image.alt=p.label;logo.append(image);b.append(logo,label);menu.append(b);}
  $('engine-current-icon').src=icons.get(s.providerId)||icons.get('DuckDuckGo')||'';
  const list=$('shortcut-list');list.replaceChildren();
  for(const item of s.items){const a=document.createElement('button');a.type='button';a.className='shortcut';a.dataset.dngItem=item.id;const icon=document.createElement('span');icon.className='shortcut-icon';icon.textContent=item.name.slice(0,1);const title=document.createElement('span');title.className='shortcut-name';title.textContent=item.name;a.append(icon,title);
   if(s.source==='custom'){const wrap=document.createElement('div');wrap.className='shortcut-wrap';const more=document.createElement('button');more.type='button';more.className='shortcut-more';more.dataset.dngMore=item.id;more.textContent='⋮';more.setAttribute('aria-label',item.name);wrap.append(a,more);list.append(wrap);}else list.append(a);}
  if(s.source==='custom'&&s.items.length<10){const add=document.createElement('button');add.type='button';add.className='shortcut shortcut-add';add.dataset.dngAdd='true';const badge=document.createElement('span');badge.className='shortcut-icon';badge.textContent='+';const label=document.createElement('span');label.className='shortcut-name';label.textContent=$('shortcut-dialog-title').textContent;add.append(badge,label);list.append(add);}
  for(const el of document.querySelectorAll('input[name="top-sites-source"]'))el.checked=el.value===s.source;
  for(const el of document.querySelectorAll('[data-clock-style]')){el.setAttribute('aria-pressed',String(el.dataset.clockStyle===v.clockStyle));el.classList.toggle('active',el.dataset.clockStyle===v.clockStyle);}
  for(const el of document.querySelectorAll('[data-clock-position]')){el.setAttribute('aria-pressed',String(el.dataset.clockPosition===v.clockPosition));el.classList.toggle('active',el.dataset.clockPosition===v.clockPosition);}
  $('shortcuts').hidden=!v.shortcuts;
  document.body.classList.toggle('plain',v.theme==='plain');document.body.classList.toggle('background-hidden',!v.backgroundVisible);
  const backgrounds={'solid-indigo':'linear-gradient(#2638b8,#2638b8)','solid-magenta':'linear-gradient(#a51d83,#a51d83)','solid-teal':'linear-gradient(#087f78,#087f78)','solid-ember':'linear-gradient(#a34420,#a34420)','gradient-violet':'linear-gradient(135deg,#251558,#7d2fe4 55%,#da3198)','gradient-aurora':'linear-gradient(135deg,#063d62,#087f78 55%,#69c67a)','gradient-ocean':'linear-gradient(135deg,#172a77,#176ca4 52%,#26b3c7)','gradient-sunset':'linear-gradient(135deg,#5b1838,#c44b43 52%,#f0a13b)'};
  document.body.style.backgroundImage=!v.backgroundVisible||v.theme==='plain'?'none':backgrounds[v.backgroundSource]?.startsWith('linear-')?backgrounds[v.backgroundSource]:v.backgroundSource.startsWith('solid-')?'none':'';
  document.body.style.backgroundColor=backgrounds[v.backgroundSource]?.startsWith('#')?backgrounds[v.backgroundSource]:'';
  for(const el of document.querySelectorAll('[data-background]'))el.setAttribute('aria-pressed',String(el.dataset.background===v.backgroundSource));
  if($('clock-format'))$('clock-format').value=v.clockFormat;
  document.documentElement.style.setProperty('--overlay',String(v.dim/100));document.documentElement.style.setProperty('--frequent-panel-alpha',String(v.frequentPanelOpacity/100));document.documentElement.style.setProperty('--frequent-icon-alpha',String(v.frequentIconOpacity/100));
  $('clock-widget').hidden=!v.clock;$('date').hidden=!v.date;$('search').hidden=!v.searchVisible;
  $('clock-widget').className='clock-widget style-'+v.clockStyle+' position-'+v.clockPosition;
  const toggles={'background-toggle':'backgroundVisible','search-toggle':'searchVisible','clock-toggle':'clock','date-toggle':'date','shortcuts-toggle':'shortcuts'};
  for(const [id,key] of Object.entries(toggles))if($(id))$(id).checked=v[key];
  for(const [id,key] of Object.entries({'dim':'dim','frequent-panel-opacity':'frequentPanelOpacity','frequent-icon-opacity':'frequentIconOpacity'}))if($(id))$(id).value=v[key];
  if($('dim-value'))$('dim-value').textContent=v.dim+'%';
  $('cards').hidden=true; // Read-only statistics are not fabricated for this engine.
  for(const id of ['background-upload','background-remove','suggestions-toggle','downloads-card-toggle','blocked-card-toggle'])if($(id))$(id).disabled=true;
  for(const el of document.querySelectorAll('[data-theme]')){el.classList.toggle('active',el.dataset.theme===v.theme);el.setAttribute('aria-pressed',String(el.dataset.theme===v.theme));}
  tick();
 }
 document.addEventListener('DngNewTabState',e=>render(e.detail));
 function validateShortcut(){let valid=false;try{const u=new URL($('shortcut-url').value.includes('://')?$('shortcut-url').value:'https://'+$('shortcut-url').value);valid=['https:','http:'].includes(u.protocol)&&!!u.hostname&&!u.username&&!u.password;}catch(e){}$('shortcut-save').disabled=!valid||!$('shortcut-name').value.trim();}
 document.addEventListener('input',e=>{if(e.target.id==='shortcut-name'||e.target.id==='shortcut-url')validateShortcut();});
 function submittedShortcut(){try{pendingShortcut={name:$('shortcut-name').value.trim(),url:new URL($('shortcut-url').value.includes('://')?$('shortcut-url').value:'https://'+$('shortcut-url').value).href};}catch(e){pendingShortcut=null;}}
 document.addEventListener('submit',e=>{e.preventDefault();if(e.target.id==='shortcut-form')submittedShortcut();});
 document.addEventListener('click',e=>{
  if(e.target.id==='customization-overlay'){e.target.hidden=true;document.body.classList.remove('customization-open');}
  if(e.target.id==='shortcut-overlay'){e.target.hidden=true;pendingShortcut=null;}
  const b=e.target.closest('button');if(!b)return;
  if(b.id==='customize'){document.body.classList.add('customization-open');$('customization-overlay').hidden=false;}
  if(b.id==='customization-close'){document.body.classList.remove('customization-open');$('customization-overlay').hidden=true;}
  if(b.dataset.category){for(const p of document.querySelectorAll('[data-category-panel]'))p.hidden=p.dataset.categoryPanel!==b.dataset.category;for(const p of document.querySelectorAll('[data-category]'))p.setAttribute('aria-selected',String(p===b));}
  if(b.id==='engine-current')$('engine-menu').hidden=!$('engine-menu').hidden;
  if(b.dataset.engine)$('engine-menu').hidden=true;
  if(b.dataset.dngMore){editing=state.items.find(i=>i.id===b.dataset.dngMore)||null;const menu=$('shortcut-menu');menu.hidden=false;menu.style.left=e.clientX+'px';menu.style.top=e.clientY+'px';}
  if(b.dataset.dngAdd||b.id==='shortcut-menu-edit'){
   if(b.dataset.dngAdd)editing=null;
   $('shortcut-name').value=editing?.name||'';$('shortcut-url').value=editing?.url||'';$('shortcut-overlay').hidden=false;$('shortcut-menu').hidden=true;$('shortcut-name').focus();validateShortcut();
  }
  if(b.id==='shortcut-menu-remove')$('shortcut-menu').hidden=true;
  if(b.id==='shortcut-save'&&!b.disabled)submittedShortcut();
  if(b.id==='shortcut-cancel'){pendingShortcut=null;$('shortcut-overlay').hidden=true;}
 });
 document.addEventListener('keydown',e=>{if(e.key==='Enter'&&['shortcut-name','shortcut-url'].includes(e.target.id)&&!$('shortcut-save').disabled)submittedShortcut();if(e.key==='Escape'){pendingShortcut=null;if($('shortcut-overlay'))$('shortcut-overlay').hidden=true;if($('shortcut-menu'))$('shortcut-menu').hidden=true;if($('customization-overlay'))$('customization-overlay').hidden=true;if($('engine-menu'))$('engine-menu').hidden=true;document.body.classList.remove('customization-open');}});
 setInterval(tick,1000);
})();
