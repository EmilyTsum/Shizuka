// SPDX-License-Identifier: GPL-2.0-only
// Run with: node tests/status_webui_test.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const elements = new Map();
const element = (id) => {
    if (!elements.has(id)) elements.set(id, {textContent: '', hidden: false, checked: false, addEventListener() {}});
    return elements.get(id);
};
const intervals = [];
let clock = 0;
const sandbox = {
    document: {hidden: true, getElementById: element, addEventListener() {}},
    window: {}, navigator: {}, Date, Promise, JSON, Number, String, Array,
    performance: {now: () => clock},
    setInterval: (fn) => intervals.push(fn), setTimeout, clearTimeout,
};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../module/webroot/status.js'), 'utf8'), sandbox);
function render(route, devices, dsp = {available: false}) {
    sandbox.snapshot = {bluetooth_enabled: true, collected_at_ms: 1, route: {transport: route, audio_mode: 0}, devices, dsp};
    vm.runInContext('render(snapshot)', sandbox);
}
const a2dp = {transport: 'A2DP', device: '<receiver>', stream_started: true, codec: {available: true, name: 'aptX Lossless (selection)', type: 12, lossless_selection: true, sample_rates_hz: [44100], bit_depths: [16], mode: 'Lossless request'}};
render('A2DP', [a2dp]);
assert.equal(element('bits').textContent, '16 bit');
assert.match(element('selection-note').textContent, /does not.*confirm lossless/);
assert.equal(element('device').textContent, '<receiver>');
assert.equal(element('stream').textContent, 'Set');
assert.equal(element('audio-mode').textContent, 'Normal');
render('A2DP', [{...a2dp, codec: {...a2dp.codec, name: 'Another codec', lossless_selection: false}}]);
assert.doesNotMatch(element('selection-note').textContent, /Lossless is selected/);
render('A2DP', [{...a2dp, stream_started: false}]);
assert.equal(element('stream').textContent, 'Clear');
const le = {transport: 'LE Audio', device: 'Earbuds', codec: {available: true, name: 'LC3', type: 0, sample_rates_hz: [48000], bit_depths: [32], frame_durations_us: [10000]}};
render('LE Audio', [a2dp, le]);
assert.equal(element('codec').textContent, 'LC3');
assert.equal(element('bits').textContent, '32 bit');
assert.equal(element('mode').textContent, '10000 µs');
assert.equal(element('stream').textContent, 'Not reported');
render('Other', [a2dp, le]);
assert.equal(element('codec').textContent, 'Not reported');
render('A2DP', [a2dp], {available: true, age_seconds: 1, encoder_state: 'ENCLEVEL_4'});
assert.equal(element('dsp-values').hidden, false);
clock = 4000;
intervals[1]();
assert.equal(element('dsp-values').hidden, true);
(async () => {
    sandbox.document.hidden = false;
    await vm.runInContext('refresh()', sandbox);
    assert.equal(element('state').textContent, 'Unavailable');
    assert.equal(element('copy').disabled, true);
    assert.equal(element('codec').textContent, 'Not reported');
    assert.equal(element('stream').textContent, '—');
    console.log('Status WebUI: route isolation, Lossless labeling, LE format, expiry and failed refresh passed.');
})().catch((error) => { console.error(error); process.exitCode = 1; });
