// SPDX-License-Identifier: GPL-2.0-only
// KernelSU's settings entry is a WebView. This page only opens the native UI.
(() => {
  "use strict";

  const command = "/data/adb/ksu/bin/busybox sh /data/adb/modules/s25_aptx_adaptive/settings.sh";
  const status = document.getElementById("launch-status");
  const button = document.getElementById("open-native");
  let initialized = false;
  let running = false;
  let sequence = 0;

  function openNative() {
    if (running) return;
    if (!window.ksu || typeof window.ksu.exec !== "function") {
      status.textContent = "Open this module in KernelSU to launch native settings, or use its Action.";
      return;
    }

    running = true;
    button.disabled = true;
    status.textContent = "Opening native Shizuka settings…";
    const callbackName = "__shizukaNativeOpen" + (++sequence);
    let finished = false;
    let timer;
    function finish(message) {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      delete window[callbackName];
      running = false;
      button.disabled = false;
      status.textContent = message;
    }
    window[callbackName] = (errno) => {
      finish(errno === 0
        ? "Native Shizuka settings opened."
        : "Could not open native settings. Try Open Shizuka, use Action, or reinstall the module.");
    };
    timer = setTimeout(() => {
      finish("Opening settings took too long. Use Action, or try Open Shizuka again.");
    }, 20000);
    try {
      // This is KernelSU's documented async exec signature. No URL, form input,
      // storage value or command output is ever used to construct a command.
      window.ksu.exec(command, "{}", callbackName);
    } catch (_) {
      finish("KernelSU could not open native settings. Use the module Action.");
    }
  }

  function initialize() {
    if (initialized) return;
    initialized = true;
    button.addEventListener("click", openNative);
    openNative();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initialize, { once: true });
  } else {
    initialize();
  }
})();
