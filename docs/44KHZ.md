# Adaptive stability investigation

The periodic click remains unresolved. Keep 48 kHz/HQ as the listening-tested stability option; it is not Lossless.

## Measured

- Both ordinary Adaptive and the separate Lossless request click at 44.1 kHz. Ordinary aptX at 44.1 kHz and Adaptive at 48 kHz/HQ did not reproduce the reported click.
- A bounded 90-second DSP producer-ring capture contained 2,925 consecutive packet observations. Two physical volume-button markers were about 56.32 seconds apart.
- The first marker followed ENCLEVEL 4→3 by about 1.47 seconds. The second was about 46.19 seconds after the last transition, while ENCLEVEL 4 remained selected. There were no packet-number discontinuities within one second of either marker; maximum host-observed gaps were about 43 ms.
- This refutes level transitions as a necessary explanation for every marked click. It does not prove correct encoded content, continuous DSP input, or successful radio delivery. Human reaction time and DSP-to-host logging delay remain unknown.
- Two later AudioFlinger snapshots had the same track underrun-frame counter. They were not synchronized to the click markers and cannot rule out an earlier underrun. Host A2DP queue counters are zero on this offload path and are not evidence of a healthy DSP queue.
- A fixed 220 Hz tone in a preloaded, looping 48 kHz PCM buffer also produced a reported click through Adaptive at 44.1 kHz/HQ. All 175 valid player timestamp observations reported zero underruns. DSP logging was off, but host status dumps ran during this test; it was not an uninstrumented listening test. The observation reduces reliance on a music app or network source as an explanation.
- The tone trace contains one roughly 440 ms presentation-timestamp offset change, not a repeated 27-second pattern. A host dump, routing or latency-report update has not been excluded. Do not treat this isolated offset as the dropout's cause.

## Code inspection

- The resynchronization setter stores analysis counters; it does not itself convert 44.1 kHz into a 27-second timer. Their downstream effects remain unverified.
- The separate `incTtp` / `decTtp` counters trigger at 700, but their callers require system mode 2 (R3) or 6 (legacy), excluding HQ modes 3/7. Multiplying 700 by a 38 ms HQ interval does not explain the reported HQ click.
- A dedicated 44.1 kHz HQ configuration exists. Absence of literal `44100` comparisons in the encoder core is not evidence that a 48 kHz table is selected.
- ENCLEVEL 3 uses a 38 ms interval, which corresponds to 1675.8 samples per channel at 44.1 kHz. Fractional accounting deserves inspection, but the second marked click occurred at ENCLEVEL 4's exact 30 ms / 1323-sample interval. The 38 ms rounding hypothesis alone does not explain all observed clicks.
- Some input/output debug strings belong to the older encoder path. Their absence from an R3 capture cannot establish that no input data was dropped.
- The core and wrapper explicitly retain remainders. The wrapper copies unread raw PCM from the core-updated cursor back to the input-buffer base; the core separately moves unused analysis-band data at chunk completion. Simple per-packet loss of `N mod 8` samples is unsupported. Queue residues must not be labeled cumulative PCM loss.
- Original signed-wrapper captures show a repeatable timing discrepancy: 3,006 consecutive 44.1 kHz/30 ms packet records regress to 1319.999 samples per packet, while nominal frame-length metadata is 1323. A separate capture gives 1320.008. The 96 kHz control follows its nominal duration within about 3 ppm. This is a strong scheduling/accounting lead, but not yet proof of decoded sample count or audible causality: the downstream COP consumer may apply its own correction.
- Lossless selection has not been verified as actual lossless transmission. Recorded encoder feedback reported QHS=0 and ENCLEVEL 3/4; these observations are not a bit-perfect test.

## Remaining boundary

Measure sample consumption and packet presentation timestamps across the R3 wrapper, DSP transport and controller, alongside receiver/link-quality counters. The ordinary Android vendor-command API rejects the reserved BQR command, including at the Bluetooth service. A private, temporary diagnostic APK allowed only UID 0 to submit one exact empty-set DELETE through the existing command queue. The controller rejected it with status `0x12` (invalid parameters); no fallback or mask changes were sent. The diagnostic APK was removed and the exact listening-tested payload restored. This does not establish whether length, zero mask, or another parameter combination caused rejection. No speculative timing patch is included.

An existing timestamped producer-ring log can be summarized locally with `python tools/analyze_encoder_log.py LOG --markers MARKERS`. This parser does not enable diagnostics or collect device data. Raw captures are not distributed.

## Additional observations

- A user also reported intermittent interruption followed by watery/compression-like artifacts at 96 kHz/24-bit. This was inferred from sound, not a bitrate display. A subsequent stable 90-second recording contained 4,461 consecutive dispatches per channel and no logged discontinuity. Encoder target bitrate changed during that stable interval; target changes alone do not establish a failure.
- HAL bitrate fields, the stock library's hardcoded 192000 callback, and padded DSP packet sizes are not measured codec or radio bitrate.
- Legacy 48/96 kHz and R3 44.1 kHz use different encoder implementations. A logged 96→44.1→48 kHz test unloaded R3, initialized the legacy encoder at 48 kHz, and received successful controller acknowledgements. The user reported normal sound. Intermittent silence on other single, slow changes remains unresolved; selecting the codec again reportedly restores it.
- DSP headers at 96 kHz contain timing-offset components of 187/197 ms. They are not receiver buffer occupancy or an acoustic latency measurement. Quick pause/stop response is inconclusive because stopping can flush queued audio. The receiver's game mode was off.
- LE scanning remained enabled in HCI traces, with advertising reports continuing after the earlier global scan-setting experiment. RF coexistence was therefore not fully excluded. No recurring 27-second scan/retry command sequence was found.

See [PIPELINE.md](PIPELINE.md) for configuration, scheduling and buffering boundaries established by code inspection.

A temporary accounting-wrapper experiment was rejected by DSP signature verification before it could run. The original wrapper was restored and its hash verified. No modified DSP library is installed. The built-in COP implementation is now being inspected offline from the original firmware.

## Later counterchecks and repairs

- The actual Adaptive graph includes SISO RAT before the encoder; matching ordinary-aptX graphs omit it. This explains why the two paths require separate investigation.
- Actual firmware RAT intermediate conversion truncates integer kHz, but the final absolute deadline uses full Hz. The previous 441-sample timer trial cannot be dismissed merely because an intermediate field becomes 10022 µs. No 44000 Hz physical clock has been established.
- A bounded 32-bit minHQ=256 trial changed the DSP's calculated adjustment to 210 ms; sound was normal but dropouts remained. It was reverted, including an exact original codec-plugin hash check. This was not a receiver-memory allocation test.
- Reconfiguration retries and controller START/STOP ownership have been repaired separately. The latest HQ-default build passed a fresh Adaptive48/HQ listening check. These changes do not establish a 44.1 kHz dropout fix.
- A review of 19 saved producer-ring captures found no normal R3 counter exposing accepted samples or represented decoded samples per output buffer. The packet log describes encoder dispatch, not a decoded-sample count. No active counter has yet been tied to the 27-second symptom; neither a clean packet sequence nor the absence of error strings proves correct downstream delivery.

## Host-to-DSP queue observation

A later fixed-tone test reproduced the reported click at Adaptive 44.1 kHz/HQ while observing the shipped GSL write-buffer bitmap and actual completion counter through read-only process memory. The tone's AudioTrack reported zero underruns. PAL source PCM was 48 kHz, stereo, in 32-bit containers, with four 7,680-byte / 20 ms periods. It was not the earlier music route's 192 kHz source configuration.

| Encoder rate | Approximate observation | Valid snapshots | Four slots in use | Completion rate |
| --- | --- | --- | --- | --- |
| 44.1 kHz, first interval | 30.80 s | 600/600 | 599 | 49.98/s |
| 44.1 kHz, second interval | 30.98 s | 599/600 | 599 | 50.01/s |
| 48 kHz control | 20.66 s | 399/400 | 399 | 50.00/s |

Snapshots were about 50 ms apart. Invalid/racing reads were excluded, not counted as empty buffers. Used slots mean reserved or DSP-unacknowledged host memory; they do not specify remaining playable samples or earbud latency. No sampled sustained host-to-DSP starvation was observed. Shorter excursions between samples remain possible, and an ACK does not prove correct downstream audio. This shifts the next observation point toward conversion, encoder output and downstream delivery. Codec rate alone must not be substituted for the actual RAT/input media format.

The signed R3 wrapper’s `decideScaleToLossless` requires 44,100 Hz and reported QHS support equal to 1, along with generation/configuration and Wi-Fi conditions. QHS=0 blocks this particular scale-up path; 44.1 kHz alone does not force Lossless. The ordinary HQ and separate Lossless selections both reach DSP HQ mode with different requests/constraints; neither proves actual lossless radio transmission. The semantic label of another required configuration word remains unconfirmed.

A fresh feedback capture without DSP binary or property changes collected no current QHS measurement because the daemon did not watch the chosen FARF directory. The temporary configuration was removed. Historical QHS=0 must not be presented as current telemetry.
