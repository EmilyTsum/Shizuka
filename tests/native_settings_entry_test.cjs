// SPDX-License-Identifier: GPL-2.0-only
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.resolve(__dirname, "..");
const source = fs.readFileSync(path.join(root, "module/webroot/launcher.js"), "utf8");
const html = fs.readFileSync(path.join(root, "module/webroot/index.html"), "utf8");
const command = "/data/adb/ksu/bin/busybox sh /data/adb/modules/s25_aptx_adaptive/settings.sh";

function page({ loading = false, bridge = true, throws = false } = {}) {
  const calls = [];
  const events = {};
  const clicks = {};
  const timers = new Map();
  let timerId = 0;
  const status = { textContent: "" };
  const button = { disabled: false, addEventListener: (name, fn) => { clicks[name] = fn; } };
  const window = { location: { search: "?command=malicious", hash: "#not-a-command" } };
  const exec = (cmd, options, callback) => {
    calls.push({ cmd, options, callback });
    if (throws) throw new Error("synthetic bridge failure");
  };
  if (bridge) window.ksu = { exec };
  const document = {
    readyState: loading ? "loading" : "complete",
    getElementById: id => {
      assert.ok(["launch-status", "open-native"].includes(id));
      return id === "launch-status" ? status : button;
    },
    addEventListener: (name, fn) => { events[name] = fn; },
  };
  const context = {
    window, document,
    setTimeout: (fn, delay) => { assert.equal(delay, 20000); timers.set(++timerId, fn); return timerId; },
    clearTimeout: id => { timers.delete(id); },
  };
  vm.runInNewContext(source, context, { timeout: 1000 });
  return { calls, events, clicks, timers, window, status, button, exec };
}

const normal = page();
assert.equal(normal.calls.length, 1);
assert.equal(normal.calls[0].cmd, command);
assert.equal(normal.calls[0].options, "{}");
assert.equal(normal.button.disabled, true);
normal.clicks.click();
assert.equal(normal.calls.length, 1, "a pending launch must not be duplicated");
const firstCallback = normal.calls[0].callback;
normal.window[firstCallback](0, "untrusted stdout", "untrusted stderr");
assert.match(normal.status.textContent, /opened/);
assert.equal(normal.button.disabled, false);
assert.equal(normal.window[firstCallback], undefined);
assert.equal(normal.timers.size, 0);
assert.deepEqual(Object.keys(normal.events), [], "resume/focus/pageshow must not auto-launch");
normal.clicks.click();
assert.equal(normal.calls.length, 2, "a user can explicitly reopen the same native UI");
assert.equal(normal.calls[1].cmd, command);
normal.window[normal.calls[1].callback](1, "<script>not executable</script>", "not a command");
assert.match(normal.status.textContent, /Could not open/);
assert.doesNotMatch(normal.status.textContent, /script/);

const loading = page({ loading: true });
assert.equal(loading.calls.length, 0);
assert.deepEqual(Object.keys(loading.events), ["DOMContentLoaded"]);
loading.events.DOMContentLoaded();
loading.events.DOMContentLoaded();
assert.equal(loading.calls.length, 1, "initialization must be idempotent");

const missing = page({ bridge: false });
assert.equal(missing.calls.length, 0);
assert.match(missing.status.textContent, /KernelSU/);
missing.window.ksu = { exec: missing.exec };
missing.clicks.click();
assert.equal(missing.calls.length, 1, "an explicit retry may use a newly available bridge");
assert.equal(missing.calls[0].cmd, command);

const failed = page({ throws: true });
assert.equal(failed.calls.length, 1);
assert.equal(failed.button.disabled, false);
assert.equal(failed.timers.size, 0);
assert.match(failed.status.textContent, /Action/);

const timeout = page();
const late = timeout.window[timeout.calls[0].callback];
[...timeout.timers.values()][0]();
assert.equal(timeout.button.disabled, false);
assert.equal(timeout.calls.length, 1, "timeout must not retry automatically");
assert.match(timeout.status.textContent, /too long/);
timeout.clicks.click();
assert.equal(timeout.calls.length, 2);
const pendingMessage = timeout.status.textContent;
late(0);
assert.equal(timeout.status.textContent, pendingMessage, "an expired callback must not overwrite a retry");
timeout.window[timeout.calls[1].callback](0);
assert.match(timeout.status.textContent, /opened/);
assert.equal(timeout.calls.every(call => call.cmd === command && call.options === "{}"), true);

assert.match(html, /<html lang="en">/);
assert.match(html, /Content-Security-Policy/);
assert.match(html, /default-src 'none'/);
assert.equal((html.match(/<script\b/g) || []).length, 1);
assert.match(html, /<script src="launcher\.js" defer><\/script>/);
assert.doesNotMatch(html, /<(?:input|select|textarea|form|iframe|img)\b/i);
assert.doesNotMatch(source, /\b(?:fetch|XMLHttpRequest|eval|innerHTML|localStorage|sessionStorage)\b/);
assert.doesNotMatch(source, /(?:window|document)\.location/);

console.log("KernelSU native entry: fixed command, one automatic launch, manual retry, errors, timeout, stale callbacks and no web settings passed");
