# Adaptive pipeline investigation

Static inspection applies to the supported Samsung firmware and its exact binary inputs. It establishes code paths, not audible correctness or receiver buffer occupancy. The 44.1 kHz click and intermittent rate-change silence remain unresolved.

```mermaid
flowchart TD
    Settings[Developer Options preference] --> JNI[Samsung Bluetooth JNI / Shizuka]
    JNI --> AV[BTIF / BTA AV reconfiguration]
    AV --> Peer[AVDTP receiver negotiation]
    AV --> HAL[Samsung audio HAL configuration]
    HAL --> PAL[Codec packer / PAL graph]
    PAL --> RAT[Rate-adapted timer]
    RAT --> PCM[PCM converter]
    PCM --> Encoder[Adaptive encoder wrapper]
    Encoder --> CP[Connection proxy sink]
    CP --> Controller[Bluetooth controller]
    CP -. Internal transport implementation .-> COP[Versioned packetizer / driver]
    COP -.-> Controller
    Controller --> Sink[Receiver decoder / playout]
    AV --> QTI[QTI offload START / STOP]
    QTI --> Controller
```

## Configuration is not an applied-state acknowledgement

Settings refreshes its store from the current codec status, changes a field, submits a preference, and updates the dialog optimistically. The native request queues asynchronous reconfiguration. Shizuka's desired state, the locally selected codec descriptor, the HAL graph and the controller session can therefore represent different stages of the same change.

Samsung's same-codec path intentionally returns an outer `restart_output=false` even when the local setter requested a restart. Actual reconfiguration is deferred through `start_session_delayed → SetCodecDeviceReconfig`. Codec reselection is a different operation: Settings resets rate/format preferences to the highest selectable values. Its ability to restore sound does not identify which deferred stage stalled.

A successful recorded 96→44.1→48 kHz transition unloaded R3, initialized the legacy encoder at 48 kHz and received successful controller responses. Android published the 48 kHz selection roughly one second before the logged DSP initialization. That successful case does not explain the user's intermittent silence.

Software A2DP transmit flags and software-queue flush errors are not sufficient failure evidence on hardware offload: Samsung bypasses those software paths. Likewise, the stock close routine can log a missing acknowledgement on an out-of-state fast path without waiting for one. A failed transition must be captured with the prior session state and actual graph/controller lifecycle preserved.

## Sample rate and PCM format

The wrapper selects R3 for the Adaptive revision-3 / 44.1 kHz combination. The inspected 48/96 kHz paths use the legacy encoder. R3's input conversion shifts interleaved 32-bit PCM containers into channel buffers; that operation changes precision/layout, not sample rate. Raw PCM and analysis-band remainders are retained separately.

The calibration database opened by the actual HAL has RAT before the PCM converter and Adaptive encoder, followed by a connection proxy sink (`070010E8`). Matching ordinary-aptX graphs omit RAT. The proxy can select internal transport implementations, including COP v0; it is not a standalone COP v0 module in these graphs.

Non-proxy PAL code distinguishes codec rate from a doubled bus rate. The actual configuration enables connectivity proxy and bypasses that host-side doubling branch. The live bus format must therefore be established through the proxy/kernel path. Neither branch enables an 88.2 kHz codec selection.

The encoder emits COP metadata for PCM frame length (`0x0A001037`), PCM format (`0x0A001038`) and zero padding (`0x0A001039`). R3 passes a nominal per-channel sample target as frame-length metadata. Actual core consumption and encoded represented samples still need to be reconciled with that target; neither buffer alignment nor a nominal interval proves a sample loss. For a nominal 1323-sample target, direct and eight-band paths use different aligned lower/upper thresholds. Analysis data and entropy bytes can carry into later chunks/output buffers. A simple claim that every output contains 1320 samples while advertising 1323 is unsupported.

## What the latency fields actually do

The inspected legacy and R3 INIT handlers accept `minHQ`, but do not read the incoming `maxHQ`. Their PROFILE handlers acknowledge the parameter as deprecated without applying the supplied TTP fields. Thus a successful parameter call does not establish that the receiver's profile values took effect.

For HQ state 3, the inspected intermediate timing calculation is:

```text
candidate_us = profile_target * 4000 - (period_ms + 1) * 1000
               - 62000 - pending_pcm_duration_us
lower_us = accepted_clamped_minHQ * 1000 + 10000
upper_us = internal_maxHQ * 4000 + 10000
adjustment = clamp(candidate_us, lower_us, upper_us)
```

The initialized target is 70. With no pending PCM, 30/20 ms periods give 187/197 ms components. A separate state-machine term can add 0/6 ms afterwards. Other conditional minimums and a legacy output-path addition can also apply. The returned header field is 16-bit in 8 µs units. These are scheduling adjustments, not measured receiver occupancy or allocated buffer sizes.

The current Lossless request retains HQ bounds 60/100. `minHQ=60` produces a 70 ms lower bound, below those observed candidates; the incoming maximum is ignored. Samsung also sign-extends the source's buffering bytes, so simply increasing a byte above 127 can reset the DSP minimum to its default rather than increase it. These fields are not a demonstrated stability control.

R3 also acknowledges the legacy bitrate-level and level-map APIs as not implemented. Its implemented IMCL controller-feedback path changes quality/ENCLEVEL; forcing that feedback is not a verified method to increase headroom. No safe, effective public control for enlarging the receiver's buffer has been established. A guarded 44.1 kHz/HQ plugin trial supplied a 32-bit minimum of 256; the DSP calculation changed to 210 ms, consistent with the minimum being clamped to internal maximum 200. Sound was normal but dropouts remained. The trial was removed and the original plugin hash verified.

The active connection proxy also consumes `0x0A001065`, but this is a boolean already enabled by default. The inspected CE driver's `fill_prebuffer` implementation only logs a message. Enabling it does not demonstrate a buffered reserve.

The wrapper requests disabling the framework's one-time prebuffer during its initial legacy initialization. Public AudioReach code maps this to inter-container buffering, with separate regular/prebuffer decisions. It is distinct from receiver playout buffering. Later switch to R3 and the stock framework's effective graph state require runtime validation. R3 at 44.1 kHz also requests COP zero padding off; HQ at 48 kHz can request it on. The closed packetizer's interpretation has not been established, so neither flag is patched speculatively.

## Remaining unobserved boundary

Completed encoder output is not final over-air delivery. Its proprietary eight-byte header is not established as an RTP header. The padded output container size, HAL bitrate and encoder target bitrate are not measured codec/radio throughput.

An offline observer has been built to inspect completed output from both encoder backends. It has not been deployed or validated for loader acceptance, runtime ABI or logging overhead. Output timestamps may still precede framework propagation; validity flags must be checked. Capturing exact per-core consumption and downstream scheduling remains necessary before changing timing or buffering behavior.

For an intermittent silent transition, preserve the first failure before reconnecting or selecting the codec again:

| Last confirmed milestone | Next boundary to examine |
| --- | --- |
| Settings selection only | Java/native preference submission |
| Local configuration changed | Deferred device reconfiguration / AVDTP |
| AVDTP reconfiguration accepted | Provider/PAL restart trigger |
| AVDTP START and PAL success | Controller offload START completion |
| Controller completion succeeded | New DSP format, source PCM and graph generation |
| Correct PCM reaches encoder | Completed encoded output and sample accounting |
| Completed encoded output continues | COP/controller delivery, encoded content and receiver playout |

The previous transition log filter selected literal `PAL`, missing tags such as `PAL: Bluetooth`; graph call ordering cannot be inferred from those missing lines. Reading existing logs and filtering tag prefixes on the host avoids changing runtime logging settings.

Public interface references: [AudioReach metadata](https://github.com/AudioReach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/interfaces/module/metadata/api/bt_metadata.h), [Bluetooth framework extension](https://github.com/AudioReach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/interfaces/module/capi/capi_fwk_extns_bt_codec.h), [codec packer](https://github.com/AudioReach/audioreach-pal/blob/f998d7b63181d59410aea4d3d79014751a24b391/plugins/codecs/bt_aptx.c). Their presence in public source does not prove identical firmware behavior.

## Fractional-rate countercheck

The actual SISO RAT firmware contains intermediate `rate / 1000` truncation, but its final absolute timer divides `frame_samples * 1000000 * counter` by the full sample rate. Thus 441 samples schedule 10000 µs before drift adjustments, even when an intermediate duration field says 10022 µs. Threshold events also use sample counts with full-rate ratios. The static threshold query has a separate rounding path, but no resulting periodic loss has been established. Integer-kHz arithmetic alone is not proof of 44000 Hz playback.
