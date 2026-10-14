/* Browser shortcuts while a page has keyboard focus.
 *
 * Keys typed into a page reach this chrome window first: Gecko dispatches a
 * key event to the chrome document before it forwards it to the page's
 * content process, and a preventDefault() by chrome stops the forwarding
 * (WidgetEvent::PreventDefault, widget/BasicEvents.h). The embedder sends the
 * keys of its browser shortcuts ("set_shortcuts"); such a key is kept from the
 * page and reported as a "shortcut" event, every other key reaches the page
 * unchanged. Shortcut names are the embedder's: "Ctrl+T", "Ctrl+Shift+Tab",
 * "Alt+ArrowLeft", "F5", "Ctrl++" (GeckoIpcServer::isValidShortcut).
 */
"use strict";

const DngKeys = (() => {
  const MAX_SHORTCUTS = 256;
  // Tests only: "test_key" types into the focused page (see testKey()).
  const TEST_INPUT = Services.env.get("DNG_TEST_INPUT") === "1";
  const NAMED = new Set(["Tab", "Enter", "Escape", "Backspace", "Delete", "Insert", "Home", "End",
    "PageUp", "PageDown", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Space"]);
  const FUNCTION_KEY = /^F([1-9]|1[0-9]|2[0-4])$/;
  // Text editing and window-manager keys stay with the page (as DaliNira's own
  // reserved keys), whatever the embedder sends.
  const RESERVED = new Set([
    ...["A", "C", "V", "X", "Y", "Z", "Insert", "Backspace", "Delete", "ArrowLeft", "ArrowRight", "ArrowUp",
      "ArrowDown", "Home", "End"].map(key => "Ctrl+" + key),
    ...["Z", "V", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End"].map(key => "Ctrl+Shift+" + key),
    "Shift+F10", "Alt+F4", "Alt+Tab", "Alt+Shift+Tab",
  ]);
  const MODIFIER_CODES = { Control: "ControlLeft", Alt: "AltLeft", Shift: "ShiftLeft" };
  let shortcuts = new Set();

  // A printable ASCII character, upper case: "T", "1", "+".
  function character(key) {
    if ([...key].length !== 1) return null;
    const upper = key.toUpperCase();
    return /^[!-`{-~]$/.test(upper) ? upper : null;
  }

  function keyName(key) {
    if (key === " ") return "Space";
    if (NAMED.has(key) || FUNCTION_KEY.test(key)) return key;
    return character(key);
  }

  function name(ctrl, alt, shift, key) {
    // Shift that selected a symbol is part of the symbol: "Ctrl++".
    const symbol = !NAMED.has(key) && !FUNCTION_KEY.test(key) && !/^[A-Z0-9]$/.test(key);
    return (ctrl ? "Ctrl+" : "") + (alt ? "Alt+" : "") + (shift && !symbol ? "Shift+" : "") + key;
  }

  function valid(shortcut) {
    if (typeof shortcut !== "string" || shortcut.length > 32 || RESERVED.has(shortcut)) return false;
    const match = /^(Ctrl\+)?(Alt\+)?(Shift\+)?(.+)$/.exec(shortcut);
    if (!match) return false;
    const [, ctrl, alt, shift, key] = match;
    const functionKey = FUNCTION_KEY.test(key);
    if ((!ctrl && !alt && !functionKey) || (ctrl && alt)) return false;
    if (functionKey || NAMED.has(key)) return true;
    return character(key) === key && (/^[A-Z0-9]$/.test(key) || !shift);
  }

  // The names a key event may have, best first. A non-Latin character on a
  // letter or digit key ("ф" on KeyA, "&" on Digit1) also has the key's Latin
  // name. None for text input in progress (IME), AltGr and Super.
  function candidates(event) {
    if (!event.isTrusted || event.isReplyEventFromRemoteContent || event.isComposing || event.keyCode === 229 ||
        event.metaKey || event.getModifierState("AltGraph")) {
      return [];
    }
    const { ctrlKey: ctrl, altKey: alt, shiftKey: shift } = event;
    const names = [];
    const key = keyName(event.key);
    if (key) names.push(name(ctrl, alt, shift, key));
    const physical = /^(?:Key([A-Z])|Digit([0-9]))$/.exec(event.code);
    if (physical && !/^[A-Za-z0-9]$/.test(event.key)) names.push(name(ctrl, alt, shift, physical[1] || physical[2]));
    return names.filter(valid);
  }

  function onKeyDown(event) {
    if (!shortcuts.size || !DngIpc.connected) return;
    const shortcut = candidates(event).find(candidate => shortcuts.has(candidate));
    if (!shortcut) return;
    event.preventDefault();
    event.stopPropagation();
    const tab = DngTabs.tabForBrowser(event.target) || DngTabs.tabs.get(DngTabs.activeId);
    ipcTabSend(tab, "shortcut", { shortcut, repeat: event.repeat });
  }

  function setShortcuts(list) {
    if (!Array.isArray(list) || list.length > MAX_SHORTCUTS || !list.every(valid)) throw new Error("invalid-argument");
    shortcuts = new Set(list);
  }

  // Types one key, with its modifiers held, into the active tab's focused
  // page through Gecko's own input path (nsITextInputProcessor), as a
  // keyboard would: the shortcut check above runs as for a real key.
  function testKey(tab, msg) {
    if (!TEST_INPUT) throw new Error("unknown-command");
    if (typeof msg.key !== "string" || !msg.key || msg.key.length > 32 || typeof msg.code !== "string" ||
        msg.code.length > 32 || !Array.isArray(msg.modifiers) || msg.modifiers.length > 3 ||
        msg.modifiers.some(modifier => !Object.hasOwn(MODIFIER_CODES, modifier)) ||
        new Set(msg.modifiers).size !== msg.modifiers.length || tab.id !== DngTabs.activeId) {
      throw new Error("invalid-argument");
    }
    tab.browser.focus();
    const tip = Cc["@mozilla.org/text-input-processor;1"].createInstance(Ci.nsITextInputProcessor);
    if (!tip.beginInputTransactionForTests(window)) throw new Error("command-failed");
    const modifier = key => new KeyboardEvent("", { key, code: MODIFIER_CODES[key] });
    for (const key of msg.modifiers) tip.keydown(modifier(key));
    const key = new KeyboardEvent("", { key: msg.key, code: msg.code });
    tip.keydown(key);
    tip.keyup(key);
    for (const key of [...msg.modifiers].reverse()) tip.keyup(modifier(key));
  }

  function install() {
    window.addEventListener("keydown", onKeyDown, { capture: true });
  }

  return { install, setShortcuts, testKey, get testInput() { return TEST_INPUT; } };
})();
