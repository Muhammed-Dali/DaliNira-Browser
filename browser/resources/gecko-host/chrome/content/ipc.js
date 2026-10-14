/* DaliNira Gecko Host <-> embedder IPC client, protocol v1/v2.
 *
 * The embedder (Qt shell) owns a Unix-domain socket server; the host connects
 * to it. Frames are a 4-byte big-endian length (1..MAX_FRAME) followed by a
 * UTF-8 JSON object with a string "type". The first frame is the host's
 * "hello" carrying the per-launch token; until the embedder answers "welcome"
 * nothing else is accepted. See qt-shell/PROTOCOL.md.
 *
 * Only a fixed set of commands is accepted and each is validated by the
 * caller's handler; nothing received here is ever evaluated as code.
 */
"use strict";

const DngIpc = (() => {
  const PROTOCOL = 1; // Baseline hello; welcome negotiates 1 or 2.
  const sessionId = Services.uuid.generateUUID().toString().replace(/[{}-]/g, "");
  let eventSequence = 0;
  let commandSequence = 0;
  let negotiatedProtocol = 1;
  const MAX_FRAME = 64 * 1024;
  const COMMANDS = new Set(["load", "back", "forward", "reload", "stop", "shutdown", "create_tab", "close_tab", "activate_tab", "load_url", "list_tabs", "cancel_request", "recover_tab", "set_shortcuts",
    ...(Services.env.get("DNG_TEST_INPUT") === "1" ? ["test_key"] : [])]);

  let input = null;
  let output = null;
  let binIn = null;
  let transport = null;
  let inBuf = new Uint8Array(0);
  let outBuf = "";
  let welcomed = false;
  let closed = false;
  let handlers = null;
  const encoder = new TextEncoder();
  const decoder = new TextDecoder("utf-8", { fatal: true });

  function close(reason) {
    if (closed) {
      return;
    }
    closed = true;
    try {
      transport?.close(Cr.NS_OK);
    } catch (e) {}
    handlers?.onClose?.(reason);
  }

  function flush() {
    if (closed || !outBuf.length) {
      return;
    }
    try {
      const written = output.write(outBuf, outBuf.length);
      outBuf = outBuf.slice(written);
    } catch (e) {
      if (e.result != Cr.NS_BASE_STREAM_WOULD_BLOCK) {
        close(`write failed: ${e.name || e}`);
        return;
      }
    }
    if (outBuf.length) {
      output.asyncWait({ onOutputStreamReady: flush }, 0, 0, Services.tm.mainThread);
    }
  }

  function send(type, data = {}) {
    if (closed || !output) {
      return false;
    }
    const envelope = welcomed && negotiatedProtocol === 3
      ? { sessionId, sequence: ++eventSequence } : {};
    const payload = encoder.encode(JSON.stringify({ ...data, ...envelope, type }));
    if (payload.length > MAX_FRAME) {
      dump(`DNG-IPC dropping oversized ${type} frame (${payload.length} bytes)\n`);
      return false;
    }
    const n = payload.length;
    let frame = String.fromCharCode((n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255);
    for (let i = 0; i < n; i += 8192) {
      frame += String.fromCharCode.apply(null, payload.subarray(i, i + 8192));
    }
    if (outBuf.length + frame.length > 1024 * 1024) { close("output queue limit"); return false; }
    outBuf += frame;
    flush();
    return true;
  }

  function dispatch(msg) {
    if (!welcomed) {
      if (
        msg.type !== "welcome" ||
        ![1, 2, 3].includes(msg.protocol) ||
        typeof msg.windowTag !== "string" ||
        !/^[0-9a-f]{32}$/.test(msg.windowTag) ||
        (msg.protocol === 3 && msg.sessionId !== sessionId)
      ) {
        close("first frame from embedder was not a valid welcome");
        return;
      }
      negotiatedProtocol = msg.protocol;
      welcomed = true;
      handlers.onWelcome(msg.windowTag);
      return;
    }
    if (negotiatedProtocol === 3) {
      if (msg.sessionId !== sessionId || !Number.isSafeInteger(msg.commandSequence) ||
          msg.commandSequence <= commandSequence) {
        send("error", { code: "invalid-session-or-sequence", command: String(msg.type).slice(0, 64),
          ...(typeof msg.requestId === "string" && /^[A-Za-z0-9_-]{1,64}$/.test(msg.requestId) ? { requestId: msg.requestId } : {}) });
        return;
      }
      commandSequence = msg.commandSequence;
    }
    if (!COMMANDS.has(msg.type) || (negotiatedProtocol === 1 &&
        !["load", "back", "forward", "reload", "stop", "shutdown"].includes(msg.type))) {
      send("error", { code: "unknown-command", command: String(msg.type).slice(0, 64),
        ...(typeof msg.requestId === "string" && /^[A-Za-z0-9_-]{1,64}$/.test(msg.requestId) ? { requestId: msg.requestId } : {}) });
      return;
    }
    try {
      handlers.onCommand(msg);
    } catch (e) {
      send("error", { code: "command-failed", command: msg.type, message: String(e).slice(0, 200) });
    }
  }

  function onReadable() {
    if (closed) {
      return;
    }
    let avail;
    try {
      avail = input.available();
    } catch (e) {
      close("embedder closed the connection");
      return;
    }
    if (avail > 0) {
      const chunk = new Uint8Array(binIn.readByteArray(Math.min(avail, MAX_FRAME + 4)));
      const merged = new Uint8Array(inBuf.length + chunk.length);
      merged.set(inBuf);
      merged.set(chunk, inBuf.length);
      inBuf = merged;
      while (inBuf.length >= 4) {
        const len = ((inBuf[0] << 24) >>> 0) + (inBuf[1] << 16) + (inBuf[2] << 8) + inBuf[3];
        if (len < 1 || len > MAX_FRAME) {
          close(`invalid frame length ${len}`);
          return;
        }
        if (inBuf.length < 4 + len) {
          break;
        }
        const body = inBuf.subarray(4, 4 + len);
        inBuf = inBuf.slice(4 + len);
        let msg;
        try {
          msg = JSON.parse(decoder.decode(body));
        } catch (e) {
          close("malformed frame");
          return;
        }
        if (!msg || typeof msg !== "object" || Array.isArray(msg) || typeof msg.type !== "string") {
          close("frame is not a typed JSON object");
          return;
        }
        dispatch(msg);
        if (closed) {
          return;
        }
      }
    }
    input.asyncWait({ onInputStreamReady: onReadable }, 0, 0, Services.tm.mainThread);
  }

  /**
   * @param {string} socketPath  embedder's Unix-domain socket
   * @param {string} tokenFile   file holding the per-launch token; deleted after reading
   * @param {object} h           { onWelcome(tag), onCommand(msg), onClose(reason) }
   */
  async function start(socketPath, tokenFile, h) {
    handlers = h;
    const token = (await IOUtils.readUTF8(tokenFile)).trim();
    await IOUtils.remove(tokenFile);
    if (!/^[0-9a-f]{64}$/.test(token)) {
      throw new Error("malformed IPC token");
    }
    const file = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
    file.initWithPath(socketPath);
    transport = Cc["@mozilla.org/network/socket-transport-service;1"]
      .getService(Ci.nsISocketTransportService)
      .createUnixDomainTransport(file);
    output = transport.openOutputStream(0, 0, 0).QueryInterface(Ci.nsIAsyncOutputStream);
    input = transport.openInputStream(0, 0, 0).QueryInterface(Ci.nsIAsyncInputStream);
    binIn = Cc["@mozilla.org/binaryinputstream;1"].createInstance(Ci.nsIBinaryInputStream);
    binIn.setInputStream(input);
    send("hello", { protocol: PROTOCOL, supportedProtocols: [1, 2, 3], sessionId, token, pid: Services.appinfo.processID });
    input.asyncWait({ onInputStreamReady: onReadable }, 0, 0, Services.tm.mainThread);
  }

  return {
    start,
    send,
    get sessionId() { return sessionId; },
    get protocol() { return negotiatedProtocol; },
    get connected() {
      return welcomed && !closed;
    },
  };
})();
