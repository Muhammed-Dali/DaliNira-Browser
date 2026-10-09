#pragma once

// DaliNira's page-world privacy shield, the same source in both engines:
// Chromium installs it as a profile script (the only kind a new renderer
// process has before its first document), WebKit as a document script.
// It runs at document start in every frame, in the page's own world.
//
//  * Global Privacy Control: navigator.globalPrivacyControl is true.
//  * Standard fingerprinting protection (Brave's "farbling"): values a
//    fingerprinting script reads are varied per top-level site and browser
//    session, so they are stable on a site but cannot link sites or
//    sessions: canvas and WebGL readback (a deterministic 1-bit change of a
//    few pixels), audio sample data (a tiny gain), CPU count and device
//    memory (a value at most the real one), language-list entropy, canvas and
//    SVG text metrics / FontFaceSet probes, WebGPU exposure, additional WebGL
//    limits and shader precision, high-entropy media queries, touch/platform
//    capability values, media-decoder power hints, and screen size.
//
// Placeholders: __DN_KEY__ the per-session key (hex), __DN_EXEMPT__ a JSON
// array of exempt (allowlisted or paused) sites, __DN_FARBLE__ true when
// fingerprinting protection is on.
inline constexpr auto kDaliNiraPrivacyShield = R"JS(
(()=>{'use strict';
const KEY='__DN_KEY__',EXEMPT=__DN_EXEMPT__,FARBLE=__DN_FARBLE__;
try{Object.defineProperty(Navigator.prototype,'globalPrivacyControl',{get(){return true},configurable:true,enumerable:true})}catch(_){}
if(!FARBLE)return;
let top='';
try{const a=location.ancestorOrigins;top=(a&&a.length?new URL(a[a.length-1]).hostname:location.hostname).toLowerCase()}catch(_){}
if(!top)return;
for(const h of EXEMPT)if(top===h||top.endsWith('.'+h))return;
const site=(h=>{if(/^[\d.]+$/.test(h)||h.includes(':'))return h;const p=h.split('.');if(p.length<=2)return h;
 const n=p[p.length-1].length===2&&p[p.length-2].length<=3?3:2;return p.slice(-n).join('.')})(top);
let seed=2166136261;for(const c of KEY+'|'+site){seed^=c.charCodeAt(0);seed=Math.imul(seed,16777619)>>>0}
const mix=x=>{x=(x^seed)>>>0;x=Math.imul(x^x>>>16,0x45d9f3b)>>>0;x=Math.imul(x^x>>>16,0x45d9f3b)>>>0;return(x^x>>>16)>>>0};
const unit=i=>mix(i)/4294967296;
const define=(proto,name,make)=>{try{const d=Object.getOwnPropertyDescriptor(proto,name);if(!d)return;
 if(typeof d.value==='function'){const original=d.value;Object.defineProperty(proto,name,{...d,value:make(original)})}
 else if(d.get){const original=d.get;Object.defineProperty(proto,name,{...d,get:make(original)})}}catch(_){}};
// Language is an intentional user preference, so keep the primary locale but
// remove the long ordered language list and make Intl's direct report agree.
// Formatting itself is not replaced: doing so would silently change content.
let language='en-US';try{language=Intl.getCanonicalLocales(navigator.language||'en-US')[0]||'en-US'}catch(_){}
const languages=Object.freeze([language]);
define(Navigator.prototype,'language',()=>function(){return language});
define(Navigator.prototype,'languages',()=>function(){return languages});
for(const prop of ['userLanguage','browserLanguage','systemLanguage'])
 try{if(prop in Navigator.prototype)define(Navigator.prototype,prop,()=>function(){return language})}catch(_){}
for(const name of ['Collator','DateTimeFormat','DisplayNames','ListFormat','NumberFormat','PluralRules','RelativeTimeFormat','Segmenter']){
 const C=Intl[name];if(!C)continue;
 if(C.prototype)define(C.prototype,'resolvedOptions',o=>function(...a){const r=o.apply(this,a);return{...r,locale:language}});
 if(typeof C.supportedLocalesOf==='function')define(C,'supportedLocalesOf',o=>function(locales,...a){
   const r=o.call(this,locales,...a);const base=language.split('-')[0].toLowerCase();
   return Array.isArray(r)?r.filter(x=>{const s=String(x).toLowerCase();return s===language.toLowerCase()||s===base}):r;
 });}
// Pixels: flip the low bit of one channel in about 1 of 32 pixels, chosen by
// the site's seed and the pixel's position: invisible, the same every read.
const noisePixels=data=>{for(let p=0,n=data.length>>2;p<n;p++){const h=mix(p+0x9e3779b9);if((h&31)===0)data[(p<<2)+(h>>>5)%3]^=1}return data};
const MAX=16777216;
define(CanvasRenderingContext2D.prototype,'getImageData',o=>function(...a){const d=o.apply(this,a);noisePixels(d.data);return d});
const noisyCopy=(canvas)=>{const w=canvas.width,h=canvas.height;if(!w||!h||w*h>MAX)return null;
 const c=document.createElement('canvas');c.width=w;c.height=h;const x=c.getContext('2d');x.drawImage(canvas,0,0);
 const d=Object.getPrototypeOf(x).getImageData.call(x,0,0,w,h);x.putImageData(d,0,0);return c};
define(HTMLCanvasElement.prototype,'toDataURL',o=>function(...a){const c=noisyCopy(this);return o.apply(c||this,a)});
define(HTMLCanvasElement.prototype,'toBlob',o=>function(...a){const c=noisyCopy(this);return o.apply(c||this,a)});
if(self.OffscreenCanvasRenderingContext2D)define(OffscreenCanvasRenderingContext2D.prototype,'getImageData',o=>function(...a){const d=o.apply(this,a);noisePixels(d.data);return d});
if(self.OffscreenCanvas)define(OffscreenCanvas.prototype,'convertToBlob',o=>function(...a){
 try{const w=this.width,h=this.height;if(w&&h&&w*h<=MAX){
  const oc=new OffscreenCanvas(w,h),ox=oc.getContext('2d');ox.drawImage(this,0,0);
  const d=Object.getPrototypeOf(ox).getImageData.call(ox,0,0,w,h);ox.putImageData(d,0,0);return o.apply(oc,a)}}catch(_){}
 return o.apply(this,a)});
const bucket=(value,id,steps)=>{if(typeof value!=='number'||!Number.isFinite(value))return value;const available=steps.filter(x=>x<=value);return available.length?available[mix(id)%available.length]:value};
const glLimits=new Map([[3379,[2048,4096,8192]],[34076,[2048,4096,8192]],[34921,[8,16]],[34930,[8,16]],
 [35660,[0,4,8,16]],[35661,[8,16,32]],[36347,[128,256,512]],[36348,[8,16,32]],[36349,[16,32,64]],
 [32883,[256,512,1024]],[35071,[256,512,1024]],[35375,[16384,32768,65536]],
 [35658,[32,64,128]],[35659,[32,64,128]],[35978,[16777216,33554432]],[36203,[4,8,16]],
 [35076,[4,8,16]],[35077,[4,8]],[35377,[16384,32768,65536]],[35376,[24,36,48,72]],
 [35382,[32,64,128]],[35383,[512,1024,2048]],[35385,[512,1024,2048]]]);
for(const G of [self.WebGLRenderingContext,self.WebGL2RenderingContext]){if(!G)continue;
 define(G.prototype,'readPixels',o=>function(...a){const r=o.apply(this,a);const px=a[6];if(px instanceof Uint8Array||px instanceof Uint8ClampedArray)noisePixels(px);return r});
 define(G.prototype,'getExtension',o=>function(n){const s=String(n).toLowerCase();if(s==='webgl_debug_renderer_info'||s==='webgl_debug_shaders')return null;return o.call(this,n)});
 define(G.prototype,'getSupportedExtensions',o=>function(){const r=o.call(this);return Array.isArray(r)?r.filter(x=>{const s=String(x).toLowerCase();return s!=='webgl_debug_renderer_info'&&s!=='webgl_debug_shaders'}).sort():r});
 // The unmasked GPU strings say what the masked ones do. Hardware maxima are
 // reported as deterministic, supported lower buckets, so applications can
 // safely size resources from them without learning the exact adapter.
 define(G.prototype,'getParameter',o=>function(p){if(p===37445)return o.call(this,7936);if(p===37446)return o.call(this,7937);
  const r=o.call(this,p);if(p===3386&&r&&r.length===2)return new Int32Array([bucket(r[0],p,[1024,2048,4096]),bucket(r[1],p+1,[1024,2048,4096])]);
  return glLimits.has(p)?bucket(r,p,glLimits.get(p)):r});
 define(G.prototype,'getShaderPrecisionFormat',o=>function(st,pt){
  const f=o.call(this,st,pt);if(!f)return f;
  const id=((st&0xff)<<8|(pt&0xff))>>>0;
  const rMin=bucket(f.rangeMin,id+70,[8,15,31,63,127]),rMax=bucket(f.rangeMax,id+71,[8,15,31,63,127]),prec=bucket(f.precision,id+72,[0,10,16,23]);
  return new Proxy(f,{get(t,p){if(p==='rangeMin')return rMin;if(p==='rangeMax')return rMax;if(p==='precision')return prec;return Reflect.get(t,p,t)}})});}
if(self.WebGLShaderPrecisionFormat){for(const [name,id,steps] of [['rangeMin',70,[8,15,31,63,127]],['rangeMax',71,[8,15,31,63,127]],['precision',72,[0,10,16,23]]])
 define(WebGLShaderPrecisionFormat.prototype,name,o=>function(){return bucket(o.call(this),id,steps)})}
if(self.WebGL2RenderingContext){
 define(WebGL2RenderingContext.prototype,'getInternalformatParameter',o=>function(target,fmt,pname){
  const r=o.call(this,target,fmt,pname);
  if(pname===35077&&r instanceof Int32Array)return new Int32Array([8,4,2].filter(x=>r.includes(x)));
  return r;});}
// Audio: a fixed per-site gain 1e-6..1e-5 away from 1 (-120..-100 dB, inaudible),
// applied once per buffer channel. Smaller deviations vanish in Float32
// rounding (1 ulp is about 1.2e-7 near 1.0) and would leave the real value.
const gain=1+(unit(1)<0.5?-1:1)*(1e-6+unit(5)*9e-6),done=new WeakMap();
const scale=(buffer,channel,data)=>{let seen=done.get(buffer);if(!seen)done.set(buffer,seen=new Set());if(seen.has(channel))return;seen.add(channel);for(let i=0;i<data.length;i++)data[i]*=gain};
if(self.AudioBuffer){define(AudioBuffer.prototype,'getChannelData',o=>function(c){const d=o.call(this,c);scale(this,c,d);return d});
 define(AudioBuffer.prototype,'copyFromChannel',o=>function(dest,c,...r){o.call(this,dest,c,...r);for(let i=0;i<dest.length;i++)dest[i]*=gain})}
if(self.AnalyserNode)define(AnalyserNode.prototype,'getFloatFrequencyData',o=>function(a){o.call(this,a);const k=(unit(2)*2-1)*1e-4;for(let i=0;i<a.length;i++)a[i]+=k});
// Hardware: CPU count between 2 and the real one; device memory at most the real one.
define(Navigator.prototype,'hardwareConcurrency',o=>function(){const n=o.call(this);return n>2?2+Math.floor(unit(3)*(n-1)):n});
define(Navigator.prototype,'deviceMemory',o=>function(){const m=o.call(this);const v=[2,4,8].filter(x=>x<=m);return v.length?v[Math.floor(unit(4)*v.length)]:m});
define(Navigator.prototype,'maxTouchPoints',o=>function(){return o.call(this)>0?1:0});
define(Navigator.prototype,'platform',()=>function(){return /mac/i.test(navigator.userAgent)?'MacIntel':/win/i.test(navigator.userAgent)?'Win32':'Linux x86_64'});
if(typeof Navigator.prototype.getGamepads==='function')define(Navigator.prototype,'getGamepads',()=>function(){return []});
if(typeof Navigator.prototype.getBattery==='function')define(Navigator.prototype,'getBattery',()=>function(){return Promise.resolve({charging:true,chargingTime:0,dischargingTime:Infinity,level:1,addEventListener(){},removeEventListener(){}})});
if(self.NetworkInformation){
 define(NetworkInformation.prototype,'downlink',()=>function(){return 10});
 define(NetworkInformation.prototype,'rtt',()=>function(){return 50});
 define(NetworkInformation.prototype,'effectiveType',()=>function(){return '4g'});
 define(NetworkInformation.prototype,'saveData',()=>function(){return false});}
if(self.Bluetooth?.prototype)define(Bluetooth.prototype,'getAvailability',()=>function(){return Promise.resolve(false)});
if(self.Keyboard?.prototype)define(Keyboard.prototype,'getLayoutMap',()=>function(){return Promise.reject(new DOMException('Blocked by privacy policy','SecurityError'))});
// WebGPU exposes adapter identity, feature, limit and timing surfaces before a
// page renders anything. Standard feature detection handles its absence.
try{const d=Object.getOwnPropertyDescriptor(Navigator.prototype,'gpu');if(d?.configurable)delete Navigator.prototype.gpu;
 else if(d?.get)Object.defineProperty(Navigator.prototype,'gpu',{...d,get(){return undefined}})}catch(_){}
if(self.WorkerNavigator?.prototype)try{delete WorkerNavigator.prototype.gpu}catch(_){}
try{if(navigator.gpu)delete navigator.gpu}catch(_){}
if(self.GPUAdapter?.prototype){
 define(GPUAdapter.prototype,'requestAdapterInfo',()=>function(){return Promise.resolve({vendor:'',architecture:'',device:'',description:''})});}
if(self.NavigatorUAData){define(NavigatorUAData.prototype,'platform',()=>function(){return /mac/i.test(navigator.userAgent)?'macOS':/win/i.test(navigator.userAgent)?'Windows':'Linux'});
 define(NavigatorUAData.prototype,'getHighEntropyValues',o=>function(h){return o.call(this,h).then(r=>({...r,architecture:'',bitness:'',model:'',platformVersion:'',wow64:false}))})}
if(self.MediaCapabilities){for(const name of ['decodingInfo','encodingInfo'])define(MediaCapabilities.prototype,name,o=>function(...a){return o.apply(this,a).then(r=>({...r,powerEfficient:false}))})}
// Fonts: text widths differ by a per-site, per-font amount far below a pixel,
// so width comparisons against a fallback font report every font alike.
const makeMeasureProxy=(m,fontStr)=>{
 let f=0;for(const c of String(fontStr||''))f=(Math.imul(f,31)+c.charCodeAt(0))>>>0;
 const k=1+(mix(f)/4294967296-0.5)*1e-5,metrics=new Set(['width','actualBoundingBoxLeft','actualBoundingBoxRight','fontBoundingBoxAscent','fontBoundingBoxDescent','actualBoundingBoxAscent','actualBoundingBoxDescent','emHeightAscent','emHeightDescent','hangingBaseline','alphabeticBaseline','ideographicBaseline']);
 return new Proxy(m,{get(target,p){const v=Reflect.get(target,p,target);return metrics.has(p)&&typeof v==='number'?v*k:v}});};
define(CanvasRenderingContext2D.prototype,'measureText',o=>function(t){return makeMeasureProxy(o.call(this,t),this.font)});
if(self.OffscreenCanvasRenderingContext2D)define(OffscreenCanvasRenderingContext2D.prototype,'measureText',o=>function(t){return makeMeasureProxy(o.call(this,t),this.font)});
if(self.SVGTextContentElement){
 for(const [name,id] of [['getComputedTextLength',80],['getSubStringLength',81]])define(SVGTextContentElement.prototype,name,o=>function(...a){const v=o.apply(this,a);return v*(1+(unit(id)-0.5)*1e-5)});
 if(SVGTextContentElement.prototype.getExtentOfChar)define(SVGTextContentElement.prototype,'getExtentOfChar',o=>function(...a){
  const r=o.apply(this,a);const k=1+(unit(82)-0.5)*1e-5;
  return new Proxy(r,{get(t,p){const v=Reflect.get(t,p,t);return(p==='width'||p==='height')&&typeof v==='number'?v*k:v}});});}
if(self.FontFaceSet)define(FontFaceSet.prototype,'check',o=>function(font,...a){const q=String(font).toLowerCase();
 const generic=['serif','sans-serif','monospace','cursive','fantasy','system-ui','ui-serif','ui-sans-serif','ui-monospace'];if(generic.some(x=>q.includes(x)))return o.call(this,font,...a);
 try{for(const face of this){const family=String(face.family||'').replace(/["']/g,'').toLowerCase();if(face.status==='loaded'&&family&&q.includes(family))return o.call(this,font,...a)}}catch(_){}return false});
if(typeof navigator.queryLocalFonts==='function')try{Object.defineProperty(navigator,'queryLocalFonts',{value:()=>Promise.resolve([]),configurable:true})}catch(_){}
// Preserve accessibility/user-choice queries, but suppress display-hardware
// capability probes for wide gamut, HDR and monochrome panels.
if(self.MediaQueryList)define(MediaQueryList.prototype,'matches',o=>function(){const q=String(this.media||'').toLowerCase();
 if(!q.includes('not ')&&(
   /color-gamut\s*:\s*(p3|rec2020)/.test(q)||
   /(video-)?dynamic-range\s*:\s*high/.test(q)||
   /monochrome\s*:\s*[1-9]/.test(q)||
   /inverted-colors\s*:\s*inverted/.test(q)||
   /forced-colors\s*:\s*active/.test(q)||
   /prefers-contrast\s*:\s*(more|less|custom)/.test(q)||
   /(any-)?pointer\s*:\s*coarse/.test(q)
 ))return false;
 return o.call(this)});
// Screen: a common desktop size (never smaller than the window).
const sw=()=>Math.max(1920,innerWidth),sh=()=>Math.max(1080,innerHeight);
for(const [n,f] of [['width',sw],['height',sh],['availWidth',sw],['availHeight',sh],['colorDepth',()=>24],['pixelDepth',()=>24]])
 define(Screen.prototype,n,()=>function(){return f()});
if(self.ScreenOrientation){
 define(ScreenOrientation.prototype,'angle',()=>function(){return 0});
 define(ScreenOrientation.prototype,'type',()=>function(){return 'landscape-primary'});}
if(self.WorkerNavigator?.prototype){
 define(WorkerNavigator.prototype,'language',()=>function(){return language});
 define(WorkerNavigator.prototype,'languages',()=>function(){return languages});
 define(WorkerNavigator.prototype,'hardwareConcurrency',o=>function(){const n=o.call(this);return n>2?2+Math.floor(unit(3)*(n-1)):n});
 define(WorkerNavigator.prototype,'deviceMemory',o=>function(){const m=o.call(this);const v=[2,4,8].filter(x=>x<=m);return v.length?v[Math.floor(unit(4)*v.length)]:m});
 try{delete WorkerNavigator.prototype.gpu}catch(_){}}
})();
)JS";
