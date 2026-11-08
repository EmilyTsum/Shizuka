# 44.1 kHz investigation

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
- A dedicated 44.1 kHz HQ configuration exists. Absence of literal `44100` comparisons in the encoder core is not evidence that a 48 kHz table is selected.
- ENCLEVEL 3 uses a 38 ms interval, which corresponds to 1675.8 samples per channel at 44.1 kHz. Fractional accounting deserves inspection, but the second marked click occurred at ENCLEVEL 4's exact 30 ms / 1323-sample interval. The 38 ms rounding hypothesis alone does not explain all observed clicks.
- Some input/output debug strings belong to the older encoder path. Their absence from an R3 capture cannot establish that no input data was dropped.
- The core has queued-input alignment and exposes `aptX3Encode_GetAnalysisProcessed`. Queued-input setup includes an eight-unit conversion for larger buffers. If each packet discarded rather than retained fractional/alignment remainders, both 30 ms (1323→1320) and 38 ms (1675.8→1672) could lose 100 samples per second. This is a conditional arithmetic model, not a measured loss: the input queue may retain those samples. Trace actual processed counts and wrapper timestamp advancement before changing intervals or padding PCM.
- Lossless selection has not been verified as actual lossless transmission. Recorded encoder feedback reported QHS=0 and ENCLEVEL 3/4; these observations are not a bit-perfect test.

## Remaining boundary

Measure sample consumption and packet presentation timestamps across the R3 wrapper, DSP transport and controller, alongside receiver/link-quality counters. The ordinary Android vendor-command API rejects the reserved BQR command, including at the Bluetooth service. No raw command was injected outside the stack's command queue. No speculative timing patch is included.

An existing timestamped producer-ring log can be summarized locally with `python tools/analyze_encoder_log.py LOG --markers MARKERS`. This parser does not enable diagnostics or collect device data. Raw captures are not distributed.
