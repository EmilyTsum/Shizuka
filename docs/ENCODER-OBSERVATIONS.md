# Encoder observations and bitrate scope

These observations describe the verified DZIF encoder module with SHA-256
`05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3`.
They do not modify the signed DSP module. Vendor binaries, disassembly and
private captures are not included in this source bundle.

## R3 frame estimate

The R3 packet summary contains `Size`, `Channels` and `Interval`. Its `Size`
is a per-channel frame-size budget, rather than the combined packet size.
The native UI labels the derived value **Encoder bitrate (frame estimate)**:

```
bit/s = Size × Channels × 8 × 1000 / Interval_ms
```

The firmware evidence is:

| Location | Evidence |
| --- | --- |
| R3 summary format at `0x1f015`, used by `aptx_adaptive3_enc_process` at `0xce84`–`0xcf20` | The `Size` argument is the integer division of the selected frame size at context offset `0x21c` by the channel count. `Interval` is the float at context offset `0x240`. |
| `encLevelHqStateMachine`, `0x13108`–`0x13160` | The interval is converted from the state machine's duration field. Multiplying it by sample rate and dividing by 1000 produces the input sample count. This establishes milliseconds. |
| `setEncoderCfgVals`, including `0x152a4`–`0x152e8` | Frame-size and interval fields are selected from encoder configuration tables before output completes. |
| R3 output paths, including `0xe558`–`0xe640` and `send_supplemental_output` | Main and supplemental output assembly, wrapper headers and conditional emission occur separately. The earlier summary is not confirmation that all those bytes were emitted or transmitted. |

For example, a summary with `Size=960`, `Channels=2` and `Interval=15.0`
corresponds to a **1,024 kb/s frame estimate**. `760`, `2`, `50.0` corresponds
to **243.2 kb/s**. These calculations describe the runtime-selected budget;
they do not count radio bytes, retransmissions, successfully delivered audio,
or receiver playout. A selected Lossless codec and an ENCLEVEL value do not
change that limitation.

The decoder calculates this field only from one complete, uniquely numbered
R3 summary containing exactly one valid size, channel count and interval. A new
summary first clears the previous tuple and estimate. Missing, duplicated,
malformed, nonfinite or out-of-bounds input cannot reuse an earlier tuple.
The current parser accepts positive sizes up to 4 MiB, one or two channels,
intervals from 1 to 120 ms, and derived rates up to 20 Mbit/s. Those are parser
bounds, not advertised codec capabilities. The result expires after three
seconds and is cleared on an encoder-context reset.

`packet_payload_bytes` remains the existing JSON field name for compatibility;
its unit is now explicitly `byte/channel` and the UI calls it **Frame size per
channel**. `encoder_frame_bitrate_bps` is the separately named derived estimate.

## Actual encoder output size

DSP Details also exposes **Encoder output buffer**, from
`encoder_output_buffer_bytes`. This is the last observed CAPI output buffer's
actual byte length, including its common wrapper. It is separate from the
selected frame budget and does not confirm downstream transport completion.

The exact stock R3 end-of-process diagnostic at `0xe96c`–`0xe9d8` reads
`output.buf_ptr[0].actual_data_len` for its **Packet size** argument. Its
**Payload size** argument is the nominal packet-type table entry, not another
actual measurement. The parser accepts exactly one complete tuple with a known
type 0–8, the reviewed nominal payload size, and actual bytes equal to that
payload plus the 8-byte wrapper. A diagnostic size mismatch invalidates the
previous output-size observation.

Legacy split-output dispatch logs separately read the completed Left/Right
CAPI output's `actual_data_len`: the Left length is published at `0x52e0` and
read at `0x52f0` onward; the Right length is published at `0x544c` and read at
`0x5468`. The parser requires the exact dispatch log, bounded unsigned packet
and cycle values, and a bounded hexadecimal pointer token. The pointer is
never included in decoded fields. A null hexadecimal pointer token does not
itself establish or reject output ownership.

Only lengths from 8 through 4096 bytes are supported by this diagnostic
parser; 4096 is a parser safety bound, not a codec maximum. Missing, duplicate,
mixed, malformed, overflowing or unsupported tuples clear the previous value.
The field expires after three seconds and clears on initialization, rate,
clock or playback-context changes. A new R3 frame-budget summary also clears
the previous output observation until a matching output-size log arrives.
A fresh output size neither sets ENCLEVEL nor proves Lossless delivery.

This reader records APSS arrival times and cannot detect a complete ring
wrap. Counting observed log bytes over that clock would therefore not establish
actual encoded transmission bitrate. The standard Bluetooth Audio AIDL
[`PresentationPosition`](https://android.googlesource.com/platform/hardware/interfaces/+/45263c60fa17f048fda4cbd715aad7a1a393c4b9/bluetooth/audio/aidl/android/hardware/bluetooth/audio/PresentationPosition.aidl)
explicitly supplies zero octets and timestamps when the software data path is
unused, including hardware offload. That zero is unavailable telemetry, not
zero radio traffic. This interface supplies no new GPR client or packet trace.

## External relay observations

The optional host-owned relay reports RD/WR completion and confirmed TTP
rewrites separately from this frame estimate. A matching WR_DONE acknowledges
the submitted encoded buffer; it does not count over-air bytes, retransmissions
or receiver-rendered samples. Those counters cannot turn the selected frame
budget into a measured transmission bitrate. Preparing a current-boot request
or mounting its library is not a completed buffer observation.

A future opaque zero-delta diagnostic would establish unchanged encoded-buffer
forwarding only. It would not identify wrapper boundaries or establish positive
delay support, actual Lossless radio delivery or receiver bit-perfect output.
Normal zero-delay settings do not enable that diagnostic operation.

## Legacy HQ/LL logs

Some encoder paths emit `br_level` feedback and a requested bitrate instead of
the R3 packet summary. They are exposed as `feedback_bitrate_level` and
`desired_encoder_bitrate_raw`. Neither is converted into ENCLEVEL, a frame
estimate, measured transmission, or a Lossless decision. The requested-bitrate
log's printed `kbps` suffix is retained in the source explanation without an
unverified numerical conversion.

## State history

ENCLEVEL values remain the encoder's exact state strings. Up to eight observed
state changes are retained per encoder context; initialization, sample-rate,
route, clock and packet-counter resets end that context. Observation times are
APSS observation times, rather than DSP-generation or audible interruption
timestamps. A state transition alone does not establish the cause of a dropout.

The status UI automatically observes while its foreground stream is eligible.
Historical measurements carry their saved time and module-session relationship
and are never promoted to current. UI freshness includes all elapsed time since
each API response, including time waiting for other calls or main-thread work.
