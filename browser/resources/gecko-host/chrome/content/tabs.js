/* Host-owned tab registry. Web content never receives this object or IPC. */
"use strict";
const DngTabs = (() => {
  const tabs = new Map();
  const MAX_TABS = 32;
  const ID = /^tab-[0-9a-f]{32}$/;
  let activeId = null;

  function notify(type, data) {
    if (DngIpc.connected && DngIpc.protocol >= 2) DngIpc.send(type, data);
  }
  function tabForBrowser(browser) {
    return [...tabs.values()].find(t => t.browser === browser);
  }
  function summary(t) {
    const s = pageState(t.browser);
    return { tabId: t.id, url: (s.uri || "").slice(0, 512),
      title: s.title.slice(0, 128), loading: !!s.loading, active: t.id === activeId };
  }
  function list(requestId) {
    notify("tabs-list", { tabs: [...tabs.values()].map(summary), activeTabId: activeId,
      ...(requestId ? { requestId } : {}) });
  }
  function activate(id) {
    const t = tabs.get(id);
    if (!t) return false;
    DngNewTab.activation();
    activeId = id;
    for (const item of tabs.values()) {
      const selected = item === t;
      item.browser.style.visibility = selected ? "visible" : "hidden";
      item.browser.docShellIsActive = selected;
      if (selected) item.browser.setAttribute("primary", "true");
      else item.browser.removeAttribute("primary");
    }
    gBrowser = t.browser;
    gLastUrl = t.lastUrl;
    gLastState = "";
    t.ipcState = "";
    t.errorDocument = ""; // Re-report an existing failure when this tab is selected.
    notify("active-tab-changed", { tabId: id });
    ipcReportState(pageState(t.browser), t);
    ipcReportGeometry();
    t.browser.focus();
    // Switching does not load a URI, replace a browser, or recreate a context.
    return true;
  }
  function create(url = "about:blank", options = {}) {
    if (tabs.size >= MAX_TABS) throw new Error("tab-limit");
    const id = "tab-" + Services.uuid.generateUUID().toString().replace(/[{}-]/g, "");
    const t = { id, browser: null, lastUrl: url, ipcState: "", errorDocument: "",
      progress: { phase: "stop", value: 1, ok: true, status: "0x0" },
      progressObjects: new Set(), listener: null, recovering: false, crashed: false };
    t.browser = createBrowser(url, options);
    tabs.set(id, t);
    notify("tab-created", { ...summary(t), source: options.source || "embedder",
      ...(options.requestId ? { requestId: options.requestId } : {}),
      ...(options.openerTabId ? { openerTabId: options.openerTabId } : {}) });
    attachProgressListener(t.browser, t);
    if (options.activate !== false || !activeId) activate(id);
    else {
      t.browser.style.visibility = "hidden";
      t.browser.docShellIsActive = false;
      t.browser.removeAttribute("primary");
    }
    if (!options.skipLoad) t.browser.fixupAndLoadURIString(url, systemLoadOptions());
    return t;
  }
  function detach(t) {
    for (const progress of t.progressObjects) {
      try { progress.removeProgressListener(t.listener); } catch (e) {}
    }
    t.progressObjects.clear();
    t.listener = null;
  }
  function close(id) {
    const t = tabs.get(id);
    if (!t) return false;
    const ids = [...tabs.keys()];
    const next = ids[ids.indexOf(id) + 1] || ids[ids.indexOf(id) - 1];
    detach(t);
    DngNewTab.retire(t);
    tabs.delete(id); // Cancels polling/recovery before destroying the frame.
    t.browser.remove();
    notify("tab-closed", { tabId: id });
    if (activeId === id) {
      activeId = null;
      if (next) activate(next);
      else create(); // The host always retains one empty tab.
    }
    return true;
  }
  function parsedURL(value) {
    if (typeof value !== "string" || !value.trim() || value.length > MAX_URL_LENGTH)
      throw new Error("invalid-argument");
    let uri;
    try {
      uri = Services.uriFixup.getFixupURIInfo(value.trim(),
        Ci.nsIURIFixup.FIXUP_FLAG_FIX_SCHEME_TYPOS).preferredURI;
    } catch (e) {}
    if (!uri || !(uri.schemeIs("http") || uri.schemeIs("https"))) throw new Error("url-rejected");
    return uri;
  }
  async function commandNow(msg) {
    const response = { command: msg.type,
      ...(typeof msg.requestId === "string" && /^[A-Za-z0-9_-]{1,64}$/.test(msg.requestId)
        ? { requestId: msg.requestId } : {}) };
    try {
      if (msg.protocol !== DngIpc.protocol || ![2, 3].includes(msg.protocol)) throw new Error("invalid-protocol");
      if (typeof msg.requestId !== "string" || !/^[A-Za-z0-9_-]{1,64}$/.test(msg.requestId))
        throw new Error("invalid-argument");
      const schemas = {
        create_tab: ["url", "activate"], close_tab: ["tabId", ...(msg.protocol === 3 ? ["discard", "documentId"] : [])], activate_tab: ["tabId"],
        load_url: ["tabId", "url"], back: ["tabId"], forward: ["tabId"],
        reload: ["tabId"], stop: ["tabId"], list_tabs: [], shutdown: msg.protocol === 3 ? ["discard", "documents"] : [],
        ...(msg.protocol === 3 ? { recover_tab: ["tabId"], cancel_request: ["targetRequestId"], set_shortcuts: ["shortcuts"],
          ...(DngKeys.testInput ? { test_key: ["tabId", "key", "code", "modifiers"], test_newtab: ["tabId", "operation"] } : {}) } : {}),
      };
      const fields = schemas[msg.type];
      if (!fields) throw new Error("unknown-command");
      if (Object.keys(msg).some(k => !["type", "protocol", "requestId", ...(msg.protocol === 3 ? ["sessionId", "commandSequence"] : []), ...fields].includes(k)))
        throw new Error("invalid-argument");
      if (msg.type === "create_tab") {
        if ("activate" in msg && typeof msg.activate !== "boolean") throw new Error("invalid-argument");
        const uri = "url" in msg ? parsedURL(msg.url) : null;
        if (tabs.size >= MAX_TABS) throw new Error("tab-limit");
        const t = create(uri?.spec || "about:blank", { activate: msg.activate !== false, requestId: msg.requestId });
        DngIpc.send("ack", { ...response, tabId: t.id });
        return;
      }
      if (msg.type === "list_tabs") { list(msg.requestId); return; }
      if (msg.type === "set_shortcuts") { DngKeys.setShortcuts(msg.shortcuts); DngIpc.send("ack", response); return; }
      if (msg.protocol === 3 && "discard" in msg && typeof msg.discard !== "boolean") throw new Error("invalid-argument");
      if (msg.type === "shutdown") {
        if (msg.protocol === 3) {
          response.documents = Object.fromEntries([...tabs.values()].map(t => [t.id, documentId(t)]));
          if (!msg.documents || typeof msg.documents !== "object" || Array.isArray(msg.documents) ||
              Object.keys(msg.documents).length !== tabs.size ||
              [...tabs.values()].some(t => !msg.documents[t.id] || msg.documents[t.id] !== documentId(t)))
            throw new Error("stale-document");
          const protectedTabs = [...tabs.values()].filter(t => !t.crashed && t.browser.hasBeforeUnload);
          if (protectedTabs.length && msg.discard !== true) throw new Error("beforeunload-confirmation-required");
          if (msg.discard === true) {
            for (const tab of protectedTabs) if (!(await tab.browser.asyncPermitUnload("unload")).permitUnload) throw new Error("unload-refused");
            if (Object.keys(msg.documents).length !== tabs.size || [...tabs.values()].some(t => msg.documents[t.id] !== documentId(t))) throw new Error("stale-document");
          }
        }
        DngIpc.send("ack", response);
        Services.startup.quit(Ci.nsIAppStartup.eAttemptQuit);
        return;
      }
      if (typeof msg.tabId !== "string" || !ID.test(msg.tabId)) throw new Error("invalid-tab-id");
      const t = tabs.get(msg.tabId);
      if (!t) throw new Error("unknown-tab");
      response.tabId = t.id;
      if (msg.protocol === 3) {
        if (msg.type === "close_tab") {
          response.documentId = documentId(t);
          if (typeof msg.documentId !== "string" || !msg.documentId || msg.documentId !== documentId(t))
            throw new Error("stale-document");
        }
        if (t.crashed && !["recover_tab", "close_tab", "activate_tab", "stop"].includes(msg.type)) throw new Error("recovery-required");
        if (["load_url", "back", "forward", "reload"].includes(msg.type) && t.browser.hasBeforeUnload)
          throw new Error("beforeunload-navigation-blocked");
        if (msg.type === "close_tab" && msg.discard === true && msg.documentId !== documentId(t)) throw new Error("stale-document");
        if (msg.type === "close_tab" && !t.crashed && t.browser.hasBeforeUnload) {
          if (msg.discard !== true) throw new Error("beforeunload-confirmation-required");
          if (msg.documentId !== documentId(t)) throw new Error("stale-document");
          const browser = t.browser;
          if (!(await browser.asyncPermitUnload("unload")).permitUnload) throw new Error("unload-refused");
          if (tabs.get(t.id) !== t || t.browser !== browser || msg.documentId !== documentId(t))
            throw new Error("stale-document");
        }
      }
      // Reject malformed navigation before revoking the live internal document.
      const navigationURI = msg.type === "load_url" ? parsedURL(msg.url) : null;
      if (msg.type === "recover_tab" && !t.crashed) throw new Error("not-crashed");
      if (["load_url", "back", "forward", "reload", "recover_tab", "stop"].includes(msg.type)) DngNewTab.intent(t, msg.type);
      switch (msg.type) {
        case "close_tab": close(t.id); break;
        case "recover_tab":
          if (!t.crashed) throw new Error("not-crashed");
          recoverBrowser(t); break;
        case "activate_tab": activate(t.id); break;
        case "load_url": {
          const uri = navigationURI; t.lastUrl = uri.spec;
          if (t.id === activeId) gLastUrl = uri.spec;
          t.browser.loadURI(uri, systemLoadOptions()); break;
        }
        case "back": case "forward":
          if (!historyStep(msg.type === "forward", t.browser)) { DngNewTab.resume(t); throw new Error("no-history"); }
          break;
        case "reload": t.browser.reload(); break;
        case "stop": t.browser.stop(); DngNewTab.resume(t); break;
        case "test_key": DngKeys.testKey(t, msg); break;
        case "test_newtab": response.data = await DngNewTab.testOperation(t,msg.operation); break;
      }
      DngIpc.send("ack", response);
    } catch (e) {
      DngIpc.send("error", { ...response, code: ["invalid-protocol", "invalid-argument", "unknown-command",
        "invalid-tab-id", "unknown-tab", "url-rejected", "tab-limit", "no-history", "beforeunload-confirmation-required",
        "beforeunload-navigation-blocked", "stale-document", "recovery-required", "not-crashed", "unload-refused"].includes(e.message)
        ? e.message : "command-failed" });
    }
  }
  function documentId(t) {
    return String(t.browser.browsingContext?.currentWindowGlobal?.innerWindowId || "");
  }
  const queued = new Map();
  function command(msg) {
    if (["load_internal_page", "internal_state", "commit_internal_action", "retire_internal"].includes(msg.type)) { DngNewTab.command(msg); return; }
    if (DngIpc.protocol !== 3) { commandNow(msg); return; }
    if (msg.type === "cancel_request") {
      if (msg.protocol !== 3 || typeof msg.requestId !== "string" || !/^[A-Za-z0-9_-]{1,64}$/.test(msg.requestId) ||
          typeof msg.targetRequestId !== "string" || !/^[A-Za-z0-9_-]{1,64}$/.test(msg.targetRequestId) ||
          Object.keys(msg).some(k => !["type", "protocol", "requestId", "sessionId", "commandSequence", "targetRequestId"].includes(k))) {
        DngIpc.send("error", { command: msg.type, code: "invalid-argument" }); return;
      }
      DngNewTab.cancel(msg.targetRequestId);
      const task = queued.get(msg.targetRequestId);
      if (task) {
        clearTimeout(task.timer); queued.delete(msg.targetRequestId);
        DngIpc.send("error", { command: task.msg.type, requestId: task.msg.requestId, code: "cancelled" });
      }
      DngIpc.send("ack", { command: "cancel_request", requestId: msg.requestId, cancelled: !!task });
      return;
    }
    // Dispatch runs on the host thread; confirmed unload awaits native responses.
    if (typeof msg.requestId !== "string" || !/^[A-Za-z0-9_-]{1,64}$/.test(msg.requestId) ||
        queued.has(msg.requestId) || queued.size >= 256) {
      DngIpc.send("error", { command: msg.type, code: "invalid-argument" }); return;
    }
    const timer = setTimeout(() => { queued.delete(msg.requestId); commandNow(msg); }, 0);
    queued.set(msg.requestId, { msg, timer });
  }
  function poll() {
    for (const t of tabs.values()) {
      const state = pageState(t.browser);
      if (state.uri && !state.uri.startsWith("about:crash") && state.uri !== "about:blank") {
        t.lastUrl = state.uri;
        if (t.id === activeId) gLastUrl = state.uri;
      }
      if (DngIpc.connected && (DngIpc.protocol >= 2 || t.id === activeId)) ipcReportState(state, t);
      attachProgressListener(t.browser, t);
    }
  }
  function welcomed() {
    for (const t of tabs.values()) {
      t.ipcState = "";
      notify("tab-created", { ...summary(t), source: "startup" });
    }
    notify("active-tab-changed", { tabId: activeId });
    list();
  }
  function nativeOpen(uri, params, where, skipLoad) {
    // Never fall back to another top-level window on refusal: throw, not null.
    if (!DngIpc.connected || DngIpc.protocol < 2 || tabs.size >= MAX_TABS ||
        !params.triggeringPrincipal || params.triggeringPrincipal.isSystemPrincipal ||
        (uri && !(uri.schemeIs("http") || uri.schemeIs("https") || uri.spec === "about:blank")))
      throw Components.Exception("page new-tab request refused", Cr.NS_ERROR_ABORT);
    const t = create(uri?.spec || "about:blank", { skipLoad: true, source: "content",
      openWindowInfo: params.openWindowInfo,
      openerTabId: tabForBrowser(params.openerBrowser)?.id,
      userContextId: params.openerOriginAttributes?.userContextId || 0,
      activate: where !== Ci.nsIBrowserDOMWindow.OPEN_NEWTAB_BACKGROUND });
    if (!skipLoad && uri) t.browser.loadURI(uri, {
      triggeringPrincipal: params.triggeringPrincipal,
      policyContainer: params.policyContainer, referrerInfo: params.referrerInfo,
      hasValidUserGestureActivation: !!params.openWindowInfo?.hasValidUserGestureActivation,
    });
    return t.browser;
  }
  function installWindowInterface() {
    window.browserDOMWindow = {
      QueryInterface: ChromeUtils.generateQI(["nsIBrowserDOMWindow"]),
      createContentWindowInFrame(uri, params, where) { return nativeOpen(uri, params, where, true); },
      openURIInFrame(uri, params, where) { return nativeOpen(uri, params, where, false); },
      createContentWindow(uri, info, where, flags, principal, policyContainer) {
        return nativeOpen(uri, { openWindowInfo: info, triggeringPrincipal: principal,
          policyContainer, openerOriginAttributes: info?.originAttributes }, where, true).browsingContext;
      },
      openURI(uri, info, where, flags, principal, policyContainer) {
        return nativeOpen(uri, { openWindowInfo: info, triggeringPrincipal: principal,
          policyContainer, openerOriginAttributes: info?.originAttributes }, where, false).browsingContext;
      },
      canClose() { return true; },
      get tabCount() { return tabs.size; },
    };
  }
  return { tabs, create, close, activate, tabForBrowser, detach, poll, welcomed, command,
    installWindowInterface, documentId, get activeId() { return activeId; } };
})();
