#!/usr/bin/env python3
"""Fixed hostile-content/native-source probes in a private host-app copy.

No arbitrary eval/test API is added to the production host. The copied app's
approved fixed script contains negative CSP probes; its actor is augmented only
with a fixed test collector to exercise actual native sender rejection.
"""
import base64,hashlib,json,os,pathlib,re,resource,shutil,subprocess,sys,tempfile,zipfile
if os.environ.get('DALINIRA_GECKO_HOST_TESTS')!='1':
 print('SKIP: requires isolated native host opt-in');sys.exit(77)
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
repo=pathlib.Path(__file__).resolve().parents[1]
source=pathlib.Path(sys.argv[1]).resolve();binary=pathlib.Path(sys.argv[2]).resolve()
root=pathlib.Path(tempfile.mkdtemp(prefix='ngs-'));root.chmod(0o700);app=root/'app';shutil.copytree(source,app)
with zipfile.ZipFile(app/'omni.ja') as archive:entries={n:archive.read(n) for n in archive.namelist()}
module='chrome/content/NewTabDocument.sys.mjs'
doc=json.loads(entries[module].decode().split('=',1)[1].strip().rstrip(';'))
script=re.search(r'<script>(.*)</script>',doc,re.S).group(1)
probe=r'''
document.addEventListener('DngNewTabState',function probe(){
 if(document.getElementById('dng-security-fixture'))return;
 setTimeout(async()=>{
 const r={},violations=new Set();document.addEventListener('securitypolicyviolation',e=>violations.add(e.effectiveDirective));const query=document.getElementById('query'),draft=query.value;query.value='synthetic denied';
 const event=new MouseEvent('click',{bubbles:true});try{Object.defineProperty(event,'isTrusted',{value:true});r.trustedImmutable=false;}catch(e){r.trustedImmutable=true;}
 document.querySelector('[data-dng-item]')?.click();
 query.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}));
 document.getElementById('search').requestSubmit();
 document.dispatchEvent(new CustomEvent('NewTab:Action',{detail:{isTrusted:true,kind:'search',query:'forged'}}));
 window.postMessage({type:'NewTab:Action',isTrusted:true,query:'forged'},'*');
 query.value=draft;
 try{eval('window.__evalRan=1');r.evalBlocked=false;}catch(e){r.evalBlocked=true;}
 const injected=document.createElement('script');injected.textContent='window.__injectedRan=1';document.body.append(injected);r.injectionBlocked=!window.__injectedRan;
 try{await fetch('http://127.0.0.1:9/');r.networkBlocked=false;}catch(e){r.networkBlocked=true;}
 try{await fetch('file:///etc/passwd');r.fileBlocked=false;}catch(e){r.fileBlocked=true;}
 await new Promise(resolve=>setTimeout(resolve,0));r.cspDirectives=[...violations];
 r.noPageAPIs=typeof Services==='undefined'&&typeof Components==='undefined'&&typeof JSWindowActorChild==='undefined'&&typeof window.navigationCapability==='undefined'&&typeof window.DngIpc==='undefined';
 const out=document.createElement('pre');out.id='dng-security-fixture';out.hidden=true;out.textContent=JSON.stringify(r);document.body.append(out);
 },30);
});
'''
combined=script+probe
new_hash=base64.b64encode(hashlib.sha256(combined.encode()).digest()).decode()
doc=doc.replace(script,combined).replace(re.search(r"sha256-([^']+)",doc).group(1),new_hash)
entries[module]=('export const NEW_TAB_DOCUMENT='+json.dumps(doc)+';\n').encode()
child='chrome/content/DaliNiraNewTabChild.sys.mjs'
text=entries[child].decode();text=text.replace("if(msg.name==='NewTab:Availability')", "if(msg.name==='NewTab:FixturePresented')return this.sendQuery('NewTab:Presented',{instance:this.instance});\n  if(msg.name==='NewTab:FixtureAttempt')return this.sendQuery('NewTab:Action',msg.data);\n  if(msg.name==='NewTab:Availability')")
entries[child]=text.encode()
parent='chrome/content/newtab.js';text=entries[parent].decode()
native=r'''
   if(operation==='fixture-idle'){for(let i=0;i<200&&fixturePending;i++)await sleep(20);if(fixturePending)throw Error('fixture pending');return {idle:true};}
   if(operation==='fixture-security'){
    const p=current(tab),wg=p.wg,other=[...documents.values()].find(x=>x!==p&&x.ready),r={};
    const proposal={instance:p.instance,revision:p.state.revision,activation:p.activation||0,sequence:p.lastProposal+1,action:{kind:'search',query:'forged source'}};
    const expected=Services.scriptSecurityManager.createContentPrincipal(Services.io.newURI(URI),{});
    r.nativePrincipal=!wg.documentPrincipal.isSystemPrincipal&&wg.documentPrincipal.isContentPrincipal&&wg.documentPrincipal.equals(expected);
    r.nativeBinding=wg===tab.browser.browsingContext.currentWindowGlobal&&wg.browsingContext.top===wg.browsingContext;
    r.replayRejected=(await wg.getActor('DaliNiraNewTab').sendQuery('NewTab:FixtureAttempt',{...proposal,sequence:p.lastProposal})).ok===false;
    r.crossTabRejected=other&&(await other.wg.getActor('DaliNiraNewTab').sendQuery('NewTab:FixtureAttempt',proposal)).ok===false;
    r.staleInstanceRejected=(await wg.getActor('DaliNiraNewTab').sendQuery('NewTab:FixtureAttempt',{...proposal,instance:'0'.repeat(32)})).ok===false;
    r.staleActivationRejected=(await wg.getActor('DaliNiraNewTab').sendQuery('NewTab:FixtureAttempt',{...proposal,activation:(p.activation||0)-1})).ok===false;
    r.staleRevisionRejected=(await wg.getActor('DaliNiraNewTab').sendQuery('NewTab:FixtureAttempt',{...proposal,revision:0})).ok===false;
    const remote=DngTabs.create(p.state.items[0].url+'?frames=1',{activate:false});
    try{
     for(let i=0;i<100;i++){if(remote.browser.browsingContext.currentWindowGlobal?.documentURI.scheme==='http'&&remote.browser.browsingContext.children.length>=2)break;await sleep(30);}
     const rwg=remote.browser.browsingContext.currentWindowGlobal;
     r.remoteRejected=(await rwg.getActor('DaliNiraNewTab').sendQuery('NewTab:FixtureAttempt',proposal)).ok===false;
     r.remoteType=rwg.domProcess.remoteType;r.contentPid=rwg.domProcess.osPid;
     r.frames=remote.browser.browsingContext.children.length;r.framesRejected=r.frames>=2;
     for(const context of remote.browser.browsingContext.children){try{context.currentWindowGlobal.getActor('DaliNiraNewTab');r.framesRejected=false;}catch(e){}}
    }finally{if(remote.browser.hasBeforeUnload)throw Error('fixture unexpectedly dirty');DngTabs.close(remote.id);}
    // A previously captured native actor is still associated with the old WG.
    // Even authentic, delayed actor traffic cannot authorize the new document.
    const oldActor=wg.getActor('DaliNiraNewTab');invalidate(tab,true);r.latePresentedRejected=(await oldActor.sendQuery('NewTab:FixturePresented',{})).ok===false&&!p.authority;tab.browser.reload();
    for(let i=0;i<100;i++){if(tab.browser.browsingContext.currentWindowGlobal!==wg&&current(tab))break;await sleep(30);}
    try{r.staleWindowGlobalRejected=(await oldActor.sendQuery('NewTab:FixtureAttempt',proposal)).ok===false;}catch(e){r.staleWindowGlobalRejected=true;}
    for(let i=0;i<100&&pageState(tab.browser).loading;i++)await sleep(30);
    ipcReportState(pageState(tab.browser),tab);r.documentId=DngTabs.documentId(tab);
    r.reloadedBound=!!current(tab)&&tab.browser.browsingContext.currentWindowGlobal!==wg;
    return r;
   }
'''
text=text.replace('const documents=new Map()', 'let fixturePending=0;const documents=new Map()')
text=text.replace('async function command(msg){','async function commandImpl(msg){')
text=text.replace('function receive(actor,msg){', 'async function command(msg){fixturePending++;try{return await commandImpl(msg);}finally{fixturePending--;}}\n function receive(actor,msg){')
text=text.replace('async function readBundle(bundleId){', 'async function readBundle(bundleId){await sleep(150);')
text=text.replace('async testOperation(tab,operation){','async testOperation(tab,operation){'+native)
entries[parent]=text.encode()
with zipfile.ZipFile(app/'omni.ja','w',compression=zipfile.ZIP_STORED) as archive:
 for name,data in entries.items():archive.writestr(name,data)
env=os.environ.copy();env['DALINIRA_GECKO_HOST_APP']=str(app)
print('Private fixed security fixture:',root,flush=True)
raise SystemExit(subprocess.call([str(binary),'--real-newtab','--security-fixture'],env=env))
