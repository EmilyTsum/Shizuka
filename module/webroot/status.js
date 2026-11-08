// SPDX-License-Identifier: GPL-2.0-only
"use strict";
const byId = (id) => document.getElementById(id);
let lastStatus = null;
let busy = false;
let counter = 0;
let receivedAt = 0;
const statusCommand = "sh /data/adb/modules/s25_aptx_adaptive/status.sh";
function text(id, value) { byId(id).textContent = value; }
function execStatus() {
    return new Promise((resolve, reject) => {
        if (!window.ksu || typeof window.ksu.exec !== "function") {
            reject(new Error("Open the Shizuka WebUI from KernelSU to read your device."));
            return;
        }
        const callback = "shizukaStatus" + (++counter);
        const timeout = setTimeout(() => { delete window[callback]; reject(new Error("Status request timed out. Try refreshing.")); }, 10000);
        window[callback] = (code, stdout, stderr) => {
            clearTimeout(timeout);
            delete window[callback];
            if (Number(code) !== 0) { reject(new Error("Status is unavailable. Check that status.dex is installed.")); return; }
            try { resolve(JSON.parse(stdout)); } catch (_) { reject(new Error("The status helper returned an invalid snapshot.")); }
        };
        try { window.ksu.exec(statusCommand, "{}", callback); }
        catch (error) { clearTimeout(timeout); delete window[callback]; reject(error); }
    });
}
function numbers(values, suffix) {
    return Array.isArray(values) && values.length ? values.map((n) => String(n) + suffix).join(" / ") : "Not reported";
}
function render(snapshot) {
    if (snapshot.error) throw new Error(snapshot.error);
    const route = snapshot.route || {};
    const devices = Array.isArray(snapshot.devices) ? snapshot.devices : [];
    const active = devices.filter((d) => d.transport === route.transport);
    const device = active[0];
    const codec = device && device.codec && device.codec.available ? device.codec : null;
    text("state", snapshot.bluetooth_enabled ? "Updated" : "Bluetooth off");
    text("transport", route.transport || "Unknown");
    text("device", active.length ? active.map((d) => d.device).join(" · ") : "No active Bluetooth device on this route");
    text("codec", codec ? codec.name : "Not reported");
    text("rate", codec ? numbers(codec.sample_rates_hz, " Hz") : "—");
    text("bits", codec ? numbers(codec.bit_depths, " bit") : "—");
    text("mode-label", route.transport === "LE Audio" ? "Frame duration" : "Mode");
    text("mode", codec ? codec.mode || numbers(codec.frame_durations_us, " µs") : "—");
    text("stream", device && typeof device.stream_started === "boolean" ? (device.stream_started ? "Set" : "Clear") : "Not reported");
    text("audio-mode", ({0: "Normal", 1: "Ringtone", 2: "In call", 3: "Communication", 4: "Call screening"})[route.audio_mode] || "Not reported");
    text("selection-note", codec && codec.type === 12 && route.transport === "A2DP" ? "Lossless is selected in Android. Its 16-bit display does not describe the DSP PCM input or confirm lossless transmission." : "The Android selection is separate from the DSP input format and actual radio transmission.");
    const dsp = snapshot.dsp || {};
    byId("dsp-values").hidden = !dsp.available;
    text("dsp-note", dsp.available ? "Fresh encoder observation; this is not a bit-perfect test." : "Not observed. " + (dsp.reason || "Diagnostics are not running."));
    if (dsp.available) {
        text("encoder", dsp.encoder_state || "Not reported");
        text("quality", String(dsp.quality_level ?? "?") + " / " + String(dsp.qhs ?? "?"));
        text("interval", dsp.packet_interval_ms == null ? "Not reported" : String(dsp.packet_interval_ms) + " ms");
        text("age", Number(dsp.age_seconds).toFixed(1) + " s");
    }
    text("message", snapshot.errors && snapshot.errors.length ? "Some status fields are unavailable on this build." : "Current media route and Android codec selection.");
    text("updated", "Updated " + new Date(snapshot.collected_at_ms).toLocaleTimeString() + " · Read-only; DSP logging stays off unless diagnostics were started separately.");
    lastStatus = snapshot;
    receivedAt = performance.now();
    byId("copy").disabled = false;
}
async function refresh() {
    if (busy || document.hidden) return;
    busy = true;
    byId("refresh").disabled = true;
    text("state", "Reading…");
    try { render(await execStatus()); }
    catch (error) { text("state", "Unavailable"); text("message", error.message || "Status is unavailable."); lastStatus = null; byId("copy").disabled = true; text("transport", "—"); text("device", "No current snapshot"); text("codec", "Not reported"); ["rate", "bits", "mode", "stream", "audio-mode"].forEach((id) => text(id, "—")); byId("dsp-values").hidden = true; text("dsp-note", "No current DSP observation."); }
    finally { busy = false; byId("refresh").disabled = false; }
}
byId("refresh").addEventListener("click", refresh);
byId("copy").addEventListener("click", async () => {
    if (!lastStatus) return;
    const value = JSON.stringify(lastStatus, null, 2);
    try {
        if (navigator.clipboard && window.isSecureContext) await navigator.clipboard.writeText(value);
        else { const field = document.createElement("textarea"); field.value = value; field.setAttribute("aria-label", "Status to copy"); document.body.appendChild(field); field.select(); const ok = document.execCommand("copy"); field.remove(); if (!ok) throw new Error("Clipboard unavailable"); }
        text("message", "Status copied. Bluetooth addresses are omitted.");
    } catch (_) { text("message", "Could not copy status in this WebView."); }
});
setInterval(() => { if (byId("auto").checked && !document.hidden) refresh(); }, 5000);
document.addEventListener("visibilitychange", () => { if (!document.hidden && byId("auto").checked) refresh(); });
refresh();

setInterval(() => {
    if (!lastStatus || !lastStatus.dsp || !lastStatus.dsp.available) return;
    const age = Number(lastStatus.dsp.age_seconds) + (performance.now() - receivedAt) / 1000;
    if (age > 3) { byId("dsp-values").hidden = true; text("dsp-note", "The last DSP observation has expired. Refresh during a diagnostic capture."); }
    else text("age", age.toFixed(1) + " s");
}, 500);
