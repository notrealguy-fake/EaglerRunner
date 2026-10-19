// shim.h - the JavaScript bridge injected into every loaded page.
//
// Provides:
//   window.eaglerNet    - TCP/UDP sockets backed by the native NetBridge
//   window.eaglerRunner - launcher/application control (open files, library, ...)
#pragma once

namespace shim {

inline const char* kJsBridge = R"JS(
(function () {
  if (window.__eaglerRunnerBridge) return;
  window.__eaglerRunnerBridge = true;

  var b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  function bytesToBase64(bytes) {
    var out = "";
    for (var i = 0; i < bytes.length; i += 3) {
      var b0 = bytes[i], b1 = i + 1 < bytes.length ? bytes[i + 1] : 0,
          b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
      out += b64chars[b0 >> 2];
      out += b64chars[((b0 & 3) << 4) | (b1 >> 4)];
      out += i + 1 < bytes.length ? b64chars[((b1 & 15) << 2) | (b2 >> 6)] : "=";
      out += i + 2 < bytes.length ? b64chars[b2 & 63] : "=";
    }
    return out;
  }

  function base64ToBytes(text) {
    var len = text.length;
    while (len > 0 && text[len - 1] === "=") len--;
    var out = new Uint8Array((len * 3) >> 2);
    var p = 0, acc = 0, nbits = 0;
    for (var i = 0; i < text.length; i++) {
      var ch = text.charAt(i);
      if (ch === "=") break;
      var d = b64chars.indexOf(ch);
      if (d < 0) continue;
      acc = (acc << 6) | d;
      nbits += 6;
      if (nbits >= 8) { nbits -= 8; out[p++] = (acc >> nbits) & 0xFF; }
    }
    return out.subarray(0, p);
  }

  function toBytes(data) {
    if (data == null) return new Uint8Array(0);
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (Array.isArray(data)) return new Uint8Array(data);
    if (typeof data === "string") {
      if (window.TextEncoder) return new TextEncoder().encode(data);
      var arr = new Uint8Array(data.length);
      for (var i = 0; i < data.length; i++) arr[i] = data.charCodeAt(i) & 0xFF;
      return arr;
    }
    return new Uint8Array(0);
  }

  function post(line) {
    try { window.chrome.webview.postMessage(line); } catch (e) {}
  }

  function enc(value) { return encodeURIComponent(String(value === undefined ? "" : value)); }

  // ---------------------------------------------------------------- networking
  var listeners = Object.create(null);
  var counter = 0;

  function emit(type, payload) {
    var list = listeners[type];
    if (list) for (var i = 0; i < list.length; i++) { try { list[i](payload); } catch (e) { console.error(e); } }
    var any = listeners["event"];
    if (any) for (var j = 0; j < any.length; j++) { try { any[j](payload); } catch (e) {} }
  }

  var net = {
    ready: true,
    on: function (type, fn) {
      (listeners[type] || (listeners[type] = [])).push(fn);
      return net;
    },
    off: function (type, fn) {
      var list = listeners[type];
      if (!list) return net;
      var i = list.indexOf(fn);
      if (i >= 0) list.splice(i, 1);
      return net;
    },
    tcp: {
      connect: function (host, port, options) {
        options = options || {};
        var id = options.id || ("tcp-" + (++counter));
        post("tcp.connect|" + enc(id) + "|" + enc(host) + "|" + enc(port | 0));
        return id;
      },
      send: function (id, data) { post("tcp.send|" + enc(id) + "|" + bytesToBase64(toBytes(data))); return id; },
      close: function (id) { post("tcp.close|" + enc(id)); return id; }
    },
    udp: {
      open: function (bindPort, options) {
        options = options || {};
        var id = options.id || ("udp-" + (++counter));
        post("udp.open|" + enc(id) + "|" + enc(bindPort | 0));
        return id;
      },
      send: function (id, host, port, data) {
        post("udp.send|" + enc(id) + "|" + enc(host) + "|" + enc(port | 0) + "|" + bytesToBase64(toBytes(data)));
        return id;
      },
      close: function (id) { post("udp.close|" + enc(id)); return id; }
    },
    toBytes: toBytes,
    fromBytes: function (data) { return data; },

    // Called by the native host for every socket event.
    _dispatch: function (line) {
      var parts = line.split("|");
      var type = parts[0];
      var id = decodeURIComponent(parts[1] || "");
      if (type === "tcp.open") emit("open", { type: "tcp", id: id });
      else if (type === "tcp.data") emit("data", { type: "tcp", id: id, data: base64ToBytes(parts[2] || "") });
      else if (type === "tcp.close") emit("close", { type: "tcp", id: id });
      else if (type === "tcp.error") emit("error", { type: "tcp", id: id, message: decodeURIComponent(parts[2] || "") });
      else if (type === "udp.open") emit("open", { type: "udp", id: id, port: parseInt(parts[2] || "0", 10) });
      else if (type === "udp.data") {
        emit("data", {
          type: "udp", id: id,
          host: decodeURIComponent(parts[2] || ""),
          port: parseInt(parts[3] || "0", 10),
          data: base64ToBytes(parts[4] || "")
        });
      }
      else if (type === "udp.close") emit("close", { type: "udp", id: id });
      else if (type === "udp.error") emit("error", { type: "udp", id: id, message: decodeURIComponent(parts[2] || "") });
      else if (type === "fatal") emit("fatal", { message: decodeURIComponent(parts[2] || "") });
    }
  };
  window.eaglerNet = net;

  // ------------------------------------------------------------- launcher API
  var appListeners = Object.create(null);
  var app = {
    send: function (command) {
      var args = Array.prototype.slice.call(arguments, 1).map(enc);
      post(["app." + command].concat(args).join("|"));
    },
    openFileDialog: function () { app.send("openDialog"); },
    openFile: function (path) { app.send("open", path); },
    openLibraryEntry: function (name) { app.send("openLibrary", name); },
    saveCurrentToLibrary: function () { app.send("addToLibrary"); },
    deleteLibraryEntry: function (name) { app.send("deleteLibrary", name); },
    showLauncher: function () { app.send("launcher"); },
    reload: function () { app.send("reload"); },
    fullscreen: function () { app.send("fullscreen"); },
    openLibraryFolder: function () { app.send("openFolder"); },
    setLowEnd: function (on) { app.send("setLowEnd", on ? 1 : 0); },
    setAskToSave: function (on) { app.send("setAsk", on ? 1 : 0); },
    refresh: function () { app.send("refresh"); },
    exit: function () { app.send("exit"); },
    on: function (type, fn) {
      (appListeners[type] || (appListeners[type] = [])).push(fn);
      return app;
    },
    // Called by the native host: payload is already JSON-encoded.
    _emit: function (type, payload) {
      var list = appListeners[type];
      if (!list) return;
      for (var i = 0; i < list.length; i++) {
        try { list[i](payload); } catch (e) { console.error(e); }
      }
    }
  };
  window.eaglerRunner = app;

  // ------------------------------------------------------- version downloader
  var dlListeners = Object.create(null);
  var downloader = {
    // Returns a Promise that resolves when the manifest is fetched.
    loadManifest: function () {
      return new Promise(function (resolve, reject) {
        var id = "manifest-load";
        post("downloader.manifest|" + enc(id));
        var done = false;
        var timeout = setTimeout(function () {
          if (!done) { done = true; reject(new Error("timeout")); }
        }, 15000);
        downloader.on("manifest", function handler(resp) {
          if (resp.id !== id) return;
          downloader.off("manifest", handler);
          clearTimeout(timeout);
          done = true;
          if (resp.error) reject(new Error(resp.error));
          else resolve(resp.versions);
        });
      });
    },
    // Start downloading a version.
    // versionId: string from manifest (e.g. "1.8.8-js", "1.12.2-wasm", "26.2")
    // Returns a Promise that resolves with the final payload
    // ({id, version, status, resultPath}) or rejects with an Error.
    // Live progress is delivered separately through the "progress" event.
    download: function (versionId) {
      return new Promise(function (resolve, reject) {
        var id = "dl-" + Date.now();
        post("downloader.download|" + enc(id) + "|" + enc(versionId));
        function onDone(resp) {
          if (resp.id !== id) return;
          downloader.off("done", onDone);
          clearTimeout(timeout);
          if (resp.error) reject(new Error(resp.error));
          else resolve(resp);
        }
        var timeout = setTimeout(function () {
          downloader.off("done", onDone);
          reject(new Error("timeout"));
        }, 600000); // 10 min
        downloader.on("done", onDone);
      });
    },
    on: function (type, fn) {
      (dlListeners[type] || (dlListeners[type] = [])).push(fn);
      return downloader;
    },
    off: function (type, fn) {
      var list = dlListeners[type];
      if (!list) return downloader;
      var i = list.indexOf(fn);
      if (i >= 0) list.splice(i, 1);
      return downloader;
    },
    _emit: function (type, payload) {
      var list = dlListeners[type];
      if (!list) return;
      for (var i = 0; i < list.length; i++) {
        try { list[i](payload); } catch (e) { console.error(e); }
      }
    }
  };
  window.eaglerDownloader = downloader;
})();
)JS";

}  // namespace shim
