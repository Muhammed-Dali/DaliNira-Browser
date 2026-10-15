/* DaliNira Gecko Host (M0 prototype).
 *
 * Owns a stack of remote content browsers in one window. All tab UI belongs
 * to the embedder; this file only loads pages and reports state as single-line JSON
 * events ("DNG-EVENT {...}") on stdout.
 *
 * Environment:
 *   DNG_START_URL  first URL to load (default about:blank)
 *   DNG_TEST=1     run the M0 scripted checks (navigation, diagnostics, crash recovery)
 *   DNG_ARTIFACTS  directory for diagnostic output
 *   DNG_DIAG_LABEL outside test mode: after the first load, write a snapshot
 *                  and diagnostics labelled with this value
 *   DNG_IPC_SOCKET / DNG_IPC_TOKEN_FILE
 *                  connect to an embedder over the IPC protocol in ipc.js
 */
"use strict";

const START_URL = Services.env.get("DNG_START_URL") || "about:blank";
const TEST_MODE = Services.env.get("DNG_TEST") === "1";
const ARTIFACTS = Services.env.get("DNG_ARTIFACTS");
const DIAG_LABEL = Services.env.get("DNG_DIAG_LABEL");
const IPC_SOCKET = Services.env.get("DNG_IPC_SOCKET");
const IPC_TOKEN_FILE = Services.env.get("DNG_IPC_TOKEN_FILE");
const MAX_URL_LENGTH = 8192;
const XHTML_NS = "http://www.w3.org/1999/xhtml";

let gBrowser = null;
let gLastState = "";
let gLastUrl = START_URL;

function emit(ev, data = {}) {
  dump(`DNG-EVENT ${JSON.stringify({ ev, t: Date.now(), ...data })}\n`);
}

function systemLoadOptions() {
  return {
    triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
  };
}

function createBrowser(url, options = {}) {
  const browser = document.createXULElement("browser");
  if (options.openWindowInfo) browser.openWindowInfo = options.openWindowInfo;
  if (options.userContextId) browser.setAttribute("usercontextid", String(options.userContextId));
  browser.permanentKey = new (Cu.getGlobalForObject(Services).Object)();
  browser.setAttribute("nodefaultsrc", "true");
  browser.setAttribute("type", "content");
  browser.setAttribute("primary", "true");
  browser.setAttribute("flex", "1");
  browser.setAttribute("maychangeremoteness", "true");
  browser.setAttribute("remote", "true");
  let remoteType = null;
  try {
    remoteType = ChromeUtils.predictRemoteTypeForURI(url, { window });
  } catch (e) {}
  browser.setAttribute("remoteType", remoteType || "web");
  browser.setAttribute("messagemanagergroup", "browsers");
  browser.setAttribute("manualactiveness", "true");
  browser.addEventListener("oop-browser-crashed", onBrowserCrashed);
  browser.addEventListener("DOMWindowClose", event => {
    // BrowserElementParent re-dispatches the native remote close request as
    // an untrusted JS event on this chrome element. Content cannot target it.
    if (event.target !== browser) return;
    event.preventDefault();
    const tab = DngTabs.tabForBrowser(browser);
    if (tab && DngIpc.protocol === 3) ipcSend("view-close-requested", { tabId: tab.id });
    else if (tab && DngIpc.protocol >= 2) DngTabs.close(tab.id);
  });
  document.getElementById("browser-stack").appendChild(browser);
  browser.preserveLayers(true);
  browser.docShellIsActive = true;
  return browser;
}

function load(url) {
  if (!url.startsWith("about:crash")) {
    gLastUrl = url;
  }
  gBrowser.fixupAndLoadURIString(url, systemLoadOptions());
}

function pageState(browser = gBrowser) {
  const bc = browser?.browsingContext;
  const wg = bc?.currentWindowGlobal;
  const sh = bc?.sessionHistory;
  return {
    uri: bc?.currentURI?.spec ?? null,
    documentURI: wg?.documentURI?.spec ?? null,
    title: browser?.contentTitle ?? "",
    loading: bc?.webProgress?.isLoadingDocument ?? null,
    pid: wg?.osPid ?? null,
    remoteType: wg?.domProcess?.remoteType ?? null,
    historyIndex: sh?.index ?? null,
    historyCount: sh?.count ?? null,
    elementCanGoBack: browser ? browser.canGoBack : null,
  };
}

function frameTree(browser = gBrowser) {
  const top = browser.browsingContext;
  return top.getAllBrowsingContextsInSubtree().map(bc => ({
    id: bc.id,
    top: bc === top,
    uri: bc.currentURI?.spec ?? null,
    pid: bc.currentWindowGlobal?.osPid ?? null,
    remoteType: bc.currentWindowGlobal?.domProcess?.remoteType ?? null,
  }));
}

function pollState() {
  const state = pageState();
  const key = JSON.stringify(state);
  if (key !== gLastState) {
    gLastState = key;
    emit("state", state);
  }
  DngTabs.poll();
}

// --- Embedder IPC ---------------------------------------------------------

function attachProgressListener(browser, tab = DngTabs.tabForBrowser(browser)) {
  const progress = browser.browsingContext?.webProgress;
  if (!tab || !progress || tab.progressObjects.has(progress)) return;
  if (!tab.listener) tab.listener = {
    QueryInterface: ChromeUtils.generateQI(["nsIWebProgressListener", "nsISupportsWeakReference"]),
    onStateChange(webProgress, request, flags, status) {
      if (tab.browser !== browser || !DngTabs.tabs.has(tab.id)) return;
      if (!webProgress?.isTopLevel || !(flags & Ci.nsIWebProgressListener.STATE_IS_WINDOW)) return;
      if (flags & Ci.nsIWebProgressListener.STATE_START) {
        tab.progress = { phase: "start", value: 0 };
      } else if (flags & Ci.nsIWebProgressListener.STATE_STOP) {
        tab.progress = { phase: "stop", value: 1, status: `0x${(status >>> 0).toString(16)}`,
          ok: Components.isSuccessCode(status) };
      } else return;
      ipcTabSend(tab, "progress", tab.progress);
    },
    onProgressChange(webProgress, request, curSelf, maxSelf, curTotal, maxTotal) {
      if (tab.browser !== browser || !DngTabs.tabs.has(tab.id)) return;
      if (webProgress?.isTopLevel && maxTotal > 0) {
        tab.progress = { phase: "change", value: Math.min(1, curTotal / maxTotal) };
        ipcTabSend(tab, "progress", tab.progress);
      }
    },
    onLocationChange() {}, onStatusChange() {}, onSecurityChange() {}, onContentBlockingEvent() {},
  };
  try {
    progress.addProgressListener(tab.listener,
      Ci.nsIWebProgress.NOTIFY_STATE_WINDOW | Ci.nsIWebProgress.NOTIFY_PROGRESS);
    tab.progressObjects.add(progress);
  } catch (e) {
    // Process switching may return a new wrapper for an already registered listener.
  }
}

function ipcTabSend(tab, type, data) {
  if (!tab || !DngTabs.tabs.has(tab.id)) return;
  if (DngIpc.protocol >= 2) ipcSend(type, { ...data, tabId: tab.id });
  else if (tab.id === DngTabs.activeId) ipcSend(type, data);
}

function ipcSend(type, data) {
  if (DngIpc.connected) {
    DngIpc.send(type, data);
  }
}

function ipcReportState(state, tab = DngTabs.tabForBrowser(gBrowser)) {
  if (!tab) return;
  const bc = tab.browser?.browsingContext;
  const mc = bc?.mediaController;
  const report = {
    url: state.uri?.slice(0, 8192),
    ...DngNewTab.report(tab),
    title: state.title.slice(0, 2048),
    loading: state.loading,
    canGoBack: state.historyIndex > 0,
    canGoForward: state.historyIndex !== null && state.historyIndex < state.historyCount - 1,
    documentURI: state.documentURI?.slice(0, 8192),
    contentPid: state.pid,
    remoteType: state.remoteType,
    mediaPlaying: mc?.isPlaying ?? false,
    mediaAudible: mc?.isAudible ?? false,
    progress: tab.progress,
    ...(DngIpc.protocol === 3 ? { documentId: DngTabs.documentId(tab),
      hasBeforeUnload: !tab.crashed && tab.browser.hasBeforeUnload, crashed: tab.crashed } : {}),
  };
  const key = JSON.stringify(report);
  if (key !== tab.ipcState) {
    tab.ipcState = key;
    ipcTabSend(tab, "state", report);
  }
  const doc = state.documentURI || "";
  if (/^about:(neterror|certerror)/.test(doc) && doc !== tab.errorDocument) {
    tab.errorDocument = doc;
    const error = new URLSearchParams(doc.slice(doc.indexOf("?") + 1)).get("e");
    ipcTabSend(tab, "load-error", { url: state.uri, error, documentURI: doc.slice(0, 512) });
  } else if (!/^about:(neterror|certerror)/.test(doc)) {
    tab.errorDocument = "";
  }
}

function ipcReportGeometry() {
  const rect = gBrowser?.getBoundingClientRect();
  ipcSend("geometry", {
    innerWidth: window.innerWidth,
    innerHeight: window.innerHeight,
    devicePixelRatio: window.devicePixelRatio,
    browserWidth: rect ? Math.round(rect.width) : null,
    browserHeight: rect ? Math.round(rect.height) : null,
  });
}

function ipcReportFocus(reason) {
  ipcSend("focus", { reason, active: Services.focus.activeWindow === window });
}

function historyStep(forward, browser = gBrowser) {
  const bc = browser.browsingContext;
  const sh = bc?.sessionHistory;
  const possible = sh && (forward ? sh.index < sh.count - 1 : sh.index > 0);
  if (!possible) {
    return false;
  }
  if (forward ? browser.canGoForward : browser.canGoBack) {
    forward ? browser.goForward() : browser.goBack();
  } else {
    forward ? bc.goForward() : bc.goBack();
  }
  return true;
}

function handleIpcCommand(msg) {
  if (DngIpc.protocol >= 2) { DngTabs.command(msg); return; }
  switch (msg.type) {
    case "load": {
      if (typeof msg.url !== "string" || !msg.url.trim() || msg.url.length > MAX_URL_LENGTH) {
        DngIpc.send("error", { code: "invalid-argument", command: "load" });
        return;
      }
      let uri = null;
      try {
        uri = Services.uriFixup.getFixupURIInfo(
          msg.url.trim(),
          Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS
        ).preferredURI;
      } catch (e) {}
      if (!uri || !(uri.schemeIs("http") || uri.schemeIs("https"))) {
        DngIpc.send("error", {
          code: "url-rejected",
          command: "load",
          url: msg.url.slice(0, 200),
          reason: "only http and https URLs are accepted",
        });
        return;
      }
      gLastUrl = uri.spec;
      DngTabs.tabForBrowser(gBrowser).lastUrl = uri.spec;
      gBrowser.loadURI(uri, systemLoadOptions());
      DngIpc.send("ack", { command: "load", url: uri.spec });
      return;
    }
    case "back":
    case "forward": {
      const ok = historyStep(msg.type === "forward");
      DngIpc.send(ok ? "ack" : "error", ok ? { command: msg.type } : { code: "no-history", command: msg.type });
      return;
    }
    case "reload":
      gBrowser.reload();
      DngIpc.send("ack", { command: "reload" });
      return;
    case "stop":
      gBrowser.stop();
      DngIpc.send("ack", { command: "stop" });
      return;
    case "shutdown":
      DngIpc.send("ack", { command: "shutdown" });
      Services.startup.quit(Ci.nsIAppStartup.eAttemptQuit);
  }
}

async function startIpc() {
  try {
    await DngIpc.start(IPC_SOCKET, IPC_TOKEN_FILE, {
      onWelcome(tag) {
        document.title = `DNG-HOST ${tag}`;
        emit("ipc-welcome", {});
        DngTabs.welcomed();
        DngIpc.send("window-tagged", { tag });
        ipcReportGeometry();
        ipcReportFocus("welcome");
      },
      onCommand: handleIpcCommand,
      onClose(reason) {
        emit("ipc-closed", { reason });
        Services.startup.quit(Ci.nsIAppStartup.eForceQuit);
      },
    });
    emit("ipc-connecting", { socket: IPC_SOCKET });
  } catch (e) {
    emit("ipc-failed", { error: String(e) });
    Services.startup.quit(Ci.nsIAppStartup.eForceQuit);
  }
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function waitFor(predicate, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const s = pageState();
      if (s.loading === false && s.pid && predicate(s)) {
        return s;
      }
    } catch (e) {}
    await sleep(100);
  }
  throw new Error(`timeout waiting for ${what}`);
}

async function writeJSON(name, data) {
  if (!ARTIFACTS) {
    return;
  }
  await IOUtils.writeJSON(PathUtils.join(ARTIFACTS, name), data, {
    tmpPath: PathUtils.join(ARTIFACTS, `${name}.tmp`),
  });
}

async function saveSnapshot(name) {
  const wg = gBrowser.browsingContext.currentWindowGlobal;
  const bitmap = await wg.drawSnapshot(null, 1, "white");
  const canvas = document.createElementNS(XHTML_NS, "canvas");
  canvas.width = bitmap.width;
  canvas.height = bitmap.height;
  canvas.getContext("2d").drawImage(bitmap, 0, 0);
  const blob = await new Promise(resolve => canvas.toBlob(resolve, "image/png"));
  const bytes = new Uint8Array(await blob.arrayBuffer());
  await IOUtils.write(PathUtils.join(ARTIFACTS, name), bytes);
  return { width: bitmap.width, height: bitmap.height };
}

function allPrefs() {
  const out = {};
  for (const name of Services.prefs.getChildList("")) {
    try {
      switch (Services.prefs.getPrefType(name)) {
        case Services.prefs.PREF_BOOL:
          out[name] = Services.prefs.getBoolPref(name);
          break;
        case Services.prefs.PREF_INT:
          out[name] = Services.prefs.getIntPref(name);
          break;
        case Services.prefs.PREF_STRING:
          out[name] = Services.prefs.getStringPref(name);
          break;
      }
    } catch (e) {}
  }
  return out;
}

function shellFacts() {
  let browserChrome = null;
  try {
    browserChrome = Cc["@mozilla.org/chrome/chrome-registry;1"]
      .getService(Ci.nsIChromeRegistry)
      .convertChromeURL(Services.io.newURI("chrome://browser/content/browser.xhtml")).spec;
  } catch (e) {
    browserChrome = `not registered (${e.name || e.result})`;
  }
  return {
    windows: [...Services.wm.getEnumerator(null)].map(w => ({
      documentURI: w.document.documentURI,
      windowtype: w.document.documentElement.getAttribute("windowtype"),
    })),
    browserChromeXhtml: browserChrome,
    commandLineHandlers: [...Services.catMan.enumerateCategory("command-line-handler")].map(
      e => `${e.entry} = ${e.value}`
    ),
    loadedFirefoxFrontendModules: Cu.loadedESModules.filter(
      m => m.startsWith("moz-src:///browser/") || m.startsWith("resource:///modules/")
    ),
    loadedModuleCount: Cu.loadedESModules.length,
    toolkitActorsRegistered: Cu.loadedESModules.includes(
      "resource://gre/modules/ActorManagerParent.sys.mjs"
    ),
    securityServices: {
      safeBrowsingModuleLoaded: Cu.loadedESModules.some(m => m.endsWith("/SafeBrowsing.sys.mjs")),
      remoteSettingsModuleLoaded: Cu.loadedESModules.some(m => m.endsWith("/RemoteSettings.sys.mjs")),
      safeBrowsingApiKey: (() => {
        try {
          const url = Services.urlFormatter.formatURLPref(
            "browser.safebrowsing.provider.google4.updateURL"
          );
          const key = new URL(url).searchParams.get("key");
          if (!key) {
            return "none";
          }
          return key === "no-google-safebrowsing-api-key"
            ? "placeholder: build has no key"
            : "present (redacted)";
        } catch (e) {
          return `<${e}>`;
        }
      })(),
    },
  };
}

function l10nFacts() {
  const tryFormat = (resources, id) => {
    try {
      return new Localization(resources, true).formatValueSync(id);
    } catch (e) {
      return `<error: ${e}>`;
    }
  };
  return {
    sources: L10nRegistry.getInstance().getSourceNames(),
    categoryEntries: [...Services.catMan.enumerateCategory("l10n-registry")].map(
      e => `${e.entry} = ${e.value}`
    ),
    appLocale: Services.locale.appLocaleAsBCP47,
    requestedLocales: Services.locale.requestedLocales,
    packagedLocales: Services.locale.packagedLocales,
    netErrorOnly: tryFormat(["toolkit/neterror/netError.ftl"], "neterror-page-title"),
    withBrand: tryFormat(
      ["branding/brand.ftl", "toolkit/neterror/netError.ftl"],
      "neterror-page-title"
    ),
  };
}

async function runtimeFacts() {
  const sandboxSettings = Cc["@mozilla.org/sandbox/sandbox-settings;1"].getService(
    Ci.mozISandboxSettings
  );
  const procInfo = await ChromeUtils.requestProcInfo();
  return {
    app: {
      name: Services.appinfo.name,
      version: Services.appinfo.version,
      platformVersion: Services.appinfo.platformVersion,
      parentPid: Services.appinfo.processID,
      browserTabsRemoteAutostart: Services.appinfo.browserTabsRemoteAutostart,
      fissionAutostart: Services.appinfo.fissionAutostart,
      sessionHistoryInParent: Services.appinfo.sessionHistoryInParent,
      chromeURI: document.documentURI,
    },
    sandbox: {
      prefContentLevel: Services.prefs.getIntPref("security.sandbox.content.level"),
      effectiveContentSandboxLevel: sandboxSettings.effectiveContentSandboxLevel,
      hasSeccompBPF: Services.sysinfo.getProperty("hasSeccompBPF"),
      hasSeccompTSync: Services.sysinfo.getProperty("hasSeccompTSync"),
      hasUserNamespaces: Services.sysinfo.getProperty("hasUserNamespaces"),
      hasPrivilegedUserNamespaces: Services.sysinfo.getProperty(
        "hasPrivilegedUserNamespaces"
      ),
    },
    updater: {
      policyStatus: Services.policies.status,
      activePolicies: Services.policies.getActivePolicies(),
      appUpdateAllowed: Services.policies.isAllowed("appUpdate"),
      alternatePath: Services.prefs.getStringPref("browser.policies.alternatePath", ""),
      startupProcessingDisabled: Services.env.get("MOZ_DISABLE_UPDATE_PROCESSING") === "1",
      serviceDisabled: Cc["@mozilla.org/updates/update-service-stub;1"]
        .getService(Ci.nsIApplicationUpdateServiceStub).updateDisabled,
      updateServiceDisabled: Cc["@mozilla.org/updates/update-service;1"]
        .getService(Ci.nsIApplicationUpdateService).disabled,
    },
    graphics: {
      windowLayerManagerType: window.windowUtils.layerManagerType,
      windowLayerManagerRemote: window.windowUtils.layerManagerRemote,
      widgetToolkit: Services.appinfo.widgetToolkit,
    },
    processes: {
      parent: { pid: procInfo.pid, type: procInfo.type },
      children: procInfo.children.map(c => ({
        pid: c.pid,
        type: c.type,
        origin: c.origin,
        childID: c.childID,
        windows: (c.windows || []).map(w => ({
          uri: w.documentURI?.spec ?? null,
          isProcessRoot: w.isProcessRoot,
        })),
      })),
    },
    frames: frameTree(),
    tabs: [...DngTabs.tabs.values()].map(t => ({ tabId: t.id, active: t.id === DngTabs.activeId,
      state: pageState(t.browser), frames: frameTree(t.browser) })),
    shell: shellFacts(),
    l10n: l10nFacts(),
  };
}

function gfxFacts() {
  const gfxInfo = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfo);
  const get = fn => {
    try {
      return fn();
    } catch (e) {
      return `<${e.name || e}>`;
    }
  };
  return {
    windowProtocol: get(() => gfxInfo.windowProtocol),
    adapterDescription: get(() => gfxInfo.adapterDescription),
    adapterDriverVendor: get(() => gfxInfo.adapterDriverVendor),
    adapterDriverVersion: get(() => gfxInfo.adapterDriverVersion),
    adapterDescription2: get(() => gfxInfo.adapterDescription2),
    info: get(() => gfxInfo.getInfo()),
    features: get(() => gfxInfo.getFeatures()),
    featureLog: get(() => gfxInfo.getFeatureLog()),
    failures: get(() => gfxInfo.getFailures({})),
  };
}

async function diagnostics(label) {
  const facts = await runtimeFacts();
  await writeJSON(`runtime-${label}.json`, facts);
  await writeJSON(`gfx-${label}.json`, gfxFacts());
  await writeJSON(`prefs-runtime-${label}.json`, allPrefs());
  // Best effort: Troubleshoot.snapshot() never settles in this app (some
  // providers wait on Firefox front-end services), so bound it.
  const { Troubleshoot } = ChromeUtils.importESModule(
    "resource://gre/modules/Troubleshoot.sys.mjs"
  );
  const snapshot = await Promise.race([
    Troubleshoot.snapshot(),
    sleep(15000).then(() => ({ timedOut: "Troubleshoot.snapshot() did not settle in 15s" })),
  ]);
  await writeJSON(`troubleshoot-${label}.json`, snapshot);
  return facts;
}

async function step(name, fn) {
  try {
    const result = await fn();
    emit("step", { name, ok: true, result });
    return result;
  } catch (e) {
    emit("step", { name, ok: false, error: String(e) });
    return null;
  }
}

async function goBackOrForward(back) {
  const before = pageState();
  if (back ? gBrowser.canGoBack : gBrowser.canGoForward) {
    back ? gBrowser.goBack() : gBrowser.goForward();
    return { via: "browser-element", before };
  }
  const bc = gBrowser.browsingContext;
  back ? bc.goBack() : bc.goForward();
  return { via: "CanonicalBrowsingContext", before };
}

async function navigationScript() {
  await step("frames-page2", async () => frameTree());
  await step("history-back", async () => {
    const how = await goBackOrForward(true);
    const s = await waitFor(x => x.uri.endsWith("/index.html"), 15000, "back");
    return { ...how, after: s };
  });
  await step("history-forward", async () => {
    const how = await goBackOrForward(false);
    const s = await waitFor(x => x.uri.endsWith("/page2.html"), 15000, "forward");
    return { ...how, after: s };
  });
  await step("load-example-org", async () => {
    load("https://example.org/");
    return waitFor(x => x.uri.startsWith("https://example.org/"), 30000, "example.org");
  });
  await step("snapshot-example-org", () => saveSnapshot("gecko-snapshot-example-org.png"));
  await step("load-expired-cert", async () => {
    load("https://expired.badssl.com/");
    const s = await waitFor(
      x => x.uri.startsWith("https://expired.badssl.com/") && x.documentURI,
      30000,
      "expired.badssl.com"
    );
    return { ...s, blocked: s.documentURI.startsWith("about:certerror") };
  });
  await step("snapshot-expired-cert", () => saveSnapshot("gecko-snapshot-certerror.png"));
  await step("load-wikipedia", async () => {
    load("https://www.wikipedia.org/");
    return waitFor(x => x.uri.startsWith("https://www.wikipedia.org/"), 30000, "wikipedia");
  });
  await sleep(1500);
  await step("snapshot-wikipedia", () => saveSnapshot("gecko-snapshot-wikipedia.png"));
  await step("diagnostics", async () => {
    const facts = await diagnostics("loaded");
    return { sandbox: facts.sandbox, graphics: facts.graphics, frames: facts.frames };
  });
  emit("diagnostics-done", pageState());
}

let gRecoveries = 0;

async function onBrowserCrashed(event) {
  const tab = DngTabs.tabForBrowser(event.currentTarget);
  if (!tab || tab.recovering) return;
  tab.recovering = true;
  tab.crashed = true;
  DngNewTab.intent(tab, "content-crashed");
  const dead = tab.browser;
  emit("content-crashed", { isTopFrame: event.isTopFrame, childID: event.childID, hostAlive: true, tabId: tab.id });
  ipcTabSend(tab, "content-crashed", { isTopFrame: event.isTopFrame });
  if (DngIpc.protocol === 3) {
    tab.recovering = false;
    ipcSend("session-state-lost", { tabId: tab.id, scope: "document", reason: "content-process-crash", stateLost: true, url: tab.lastUrl });
    tab.ipcState = "";
    ipcReportState(pageState(tab.browser), tab);
    return;
  }
  return recoverBrowser(tab);
}

async function recoverBrowser(tab) {
  if (!DngTabs.tabs.has(tab.id) || (tab.recovering && DngIpc.protocol === 3)) return;
  tab.recovering = true;
  const dead = tab.browser;
  DngTabs.detach(tab);
  tab.browser = createBrowser(tab.lastUrl);
  tab.crashed = false;
  dead.remove();
  const recoveringBrowser = tab.browser;
  if (tab.id === DngTabs.activeId) DngTabs.activate(tab.id);
  else { tab.browser.style.visibility = "hidden"; tab.browser.docShellIsActive = false; tab.browser.removeAttribute("primary"); }
  attachProgressListener(tab.browser, tab);
  tab.browser.fixupAndLoadURIString(tab.lastUrl, systemLoadOptions());
  try {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      if (!DngTabs.tabs.has(tab.id) || tab.browser !== recoveringBrowser) return;
      const s = pageState(tab.browser);
      if (s.loading === false && s.pid && s.uri && s.uri !== "about:blank") {
        gRecoveries++;
        emit("recovered", { ...s, tabId: tab.id, recoveries: gRecoveries, frames: frameTree(tab.browser) });
        ipcTabSend(tab, "recovered", { url: DngNewTab.report(tab).url || s.uri, contentPid: s.pid });
        return;
      }
      await sleep(100);
    }
    throw new Error("recovery timed out");
  } catch (e) {
    emit("recovery-failed", { tabId: tab.id, error: String(e) });
    ipcTabSend(tab, "load-error", { error: "recovery-failed", url: tab.lastUrl });
  } finally { tab.recovering = false; }
}

// Test mode: the driver creates this file to have the host crash a content
// process from the inside (MOZ_CRASH via about:crashcontent).
async function watchCrashTrigger() {
  const trigger = PathUtils.join(ARTIFACTS, ".trigger-mozcrash");
  while (true) {
    await sleep(300);
    if (await IOUtils.exists(trigger)) {
      await IOUtils.remove(trigger);
      emit("mozcrash-requested", pageState());
      load("about:crashcontent");
    }
  }
}

async function startup() {
  emit("startup", {
    parentPid: Services.appinfo.processID,
    app: Services.appinfo.name,
    gecko: Services.appinfo.platformVersion,
    testMode: TEST_MODE,
  });
  // Registers toolkit's JSWindowActors (NetError, Select, DateTimePicker,
  // Finder, ...) at module evaluation; Firefox does this from BrowserGlue and
  // GeckoView from GeckoViewStartup, neither of which runs here.
  ChromeUtils.importESModule("resource://gre/modules/ActorManagerParent.sys.mjs");

  DngTabs.installWindowInterface();
  DngTabs.create(START_URL, { source: "startup" });
  setInterval(pollState, 200);

  if (IPC_SOCKET && IPC_TOKEN_FILE) {
    window.addEventListener("resize", ipcReportGeometry);
    window.addEventListener("activate", () => ipcReportFocus("activate"));
    window.addEventListener("deactivate", () => ipcReportFocus("deactivate"));
    DngKeys.install();
    startIpc();
  }

  if (!TEST_MODE) {
    if (DIAG_LABEL && ARTIFACTS) {
      await step("initial-load", () =>
        waitFor(x => x.uri !== "about:blank", 30000, "start page")
      );
      await sleep(1500);
      await step("snapshot", () => saveSnapshot(`gecko-snapshot-${DIAG_LABEL}.png`));
      await step("diagnostics", async () => {
        const facts = await diagnostics(DIAG_LABEL);
        return { sandbox: facts.sandbox, graphics: facts.graphics };
      });
      emit("diagnostics-done", pageState());
    }
    return;
  }
  const ready = await step("initial-load", () =>
    waitFor(x => x.uri.startsWith(START_URL), 30000, "start page")
  );
  if (!ready) {
    return;
  }
  gBrowser.focus();
  await step("page-ready", () =>
    waitFor(x => x.title.includes('"ready":true'), 30000, "test page ready")
  );
  await step("frames-initial", async () => frameTree());
  await step("snapshot-initial", () => saveSnapshot("gecko-snapshot-initial.png"));
  emit("ready", pageState());
  if (ARTIFACTS) {
    watchCrashTrigger();
  }

  let navStarted = false;
  setInterval(() => {
    const s = pageState();
    if (!navStarted && s.uri?.endsWith("/page2.html") && s.loading === false) {
      navStarted = true;
      emit("link-navigation", s);
      navigationScript();
    }
  }, 200);
}

window.addEventListener("load", startup, { once: true });
