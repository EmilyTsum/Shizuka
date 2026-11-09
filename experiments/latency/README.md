# External encoded TTP adjustment

This experiment implements the byte transform for an external, runtime delay
value while retaining the stock signed DSP. It is separate from the module
build and is **not connected to live Bluetooth playback**. No experiment is
automatically applied by installing the module or opening KernelSU Action.

See the [validation record](VALIDATION.md) for completed checks and their scope.

The proposed route is the existing Adaptive encoder → signed shared-memory RD
endpoint → APSS transform → signed WR endpoint → existing logger/connection
proxy. APSS changes the encoded packet's target time to play (TTP) after
encoding. This does not require changing or re-signing the encoder image.
An actual receiver must still be tested before claiming a useful additional
buffer or retransmission budget.

## Components

- `native/` is a bounded C++17 transform and Android-compatible file CLI. Its
  API accepts the delta at runtime, once per complete RD buffer. See its
  [contract and build instructions](native/README.md).
- `python/encoded_ttp_relay.py` is the frozen independent reference transform.
  Its SHA-256 is
  `fa98734c0c79d0f31bdf642f52aa5da3ffa6413d20a541dd69ae148a972dcdaf`.
  Use the native CLI for bounded file handling; the Python entry point is a
  research oracle.
- `python/test_encoded_ttp_relay.py` runs synthetic arithmetic, format,
  metadata, timestamp and rejection checks. It has no private capture
  dependency.
- `python/endpoint_wire_protocol.py` serializes and validates endpoint wire
  bodies offline. It performs no GPR, memory allocation or graph operations.
- `python/generate_fixture.py` creates deterministic synthetic input for
  repeatable local debugging. No firmware or captured audio is included.
- [Transport and session contract](TRANSPORT-LIFECYCLE.md) records the required
  HAL ownership, buffer/cache transitions and teardown behavior for a future
  live route.

## External value and mode contract

The module already stores typed integer settings
`adaptive_hq_ttp_delta_ms` (0–150) and `adaptive_ll_ttp_delta_ms` (0–100) in its
configuration. Saving them records intent; it does not attest an applied TTP
change. The native transform can read the same file with
`--config /data/adb/shizuka/config.conf`. It snapshots the HQ setting for one
complete transform and applies the reviewed R3/HQ/44.1 kHz profile gate. It
parses the LL setting but does not enable an LL transform. The immediate parent
directory and file must belong to the effective user and have modes 0700 and
0600 respectively. Malformed or unreadable files are rejected.

This config reader belongs to the root-side file tool. The actual HAL runs as
`audioserver` and cannot read the root-owned private configuration directly.
A future in-HAL transform needs an authenticated root/HAL provider to deliver
an immutable typed value with its session authority. That provider is not
implemented. Do not make the configuration world-readable or assume the HAL
has root access.

Alternatively, pass `--delta-us` for a direct value. It is mutually exclusive
with `--config`; neither method needs a DSP or executable rebuild. For
example, `150000` means an additional 150 ms. A direct value must be an exact
multiple of 8 µs. This connects the saved setting to the file transform only;
the live transport and receiver remain unvalidated.

A live owner must independently identify the actual mode and graph epoch,
select the corresponding typed setting, and take one immutable snapshot for
the complete returned RD buffer. Split-channel wrappers in that buffer must
use the same snapshot. A settings label or a plausible packet header cannot
establish the active codec. Profile changes invalidate the previous epoch.

The implemented profile is explicitly `stock-r3-hq-44100`. LL, Lossless,
legacy framing and other sample rates are rejected. Existing INIT information
alone does not distinguish Lossless from HQ on this backend. Support for one
format must not silently enable the other modes.

## Preserved bytes and limits

For a timing-bearing wrapper, 197,000 µs plus 150,000 µs serializes as
347,000 µs. Split-TX secondary wrappers have a zero TTP sentinel and retain
zero. The transform preserves every other encoded byte, the original session
timestamp and the exact forwarded Bluetooth metadata wire bytes. It consumes
one full-buffer client frame-info record to build the WR timestamp/flags.

The 16-bit TTP word has 8-µs units and a maximum of 524,280 µs. Overflow rejects
the whole buffer before producing output. Unknown metadata, EOS, tracking,
extensions, unproved framing, malformed sizes and incomplete buffers also
reject the whole buffer. It does not scan or resynchronize through bad data.

Adding PCM samples or a signed PCM latency module would delay the sender's
audio timeline. That is a different operation from changing packet TTP; it
does not by itself establish additional receiver buffering or radio retry
time. These measurements and controls must remain distinguishable.

## Reproduce the offline checks

Run from the repository root and put generated artifacts outside the checkout:

```sh
python3 experiments/latency/python/test_encoded_ttp_relay.py \
  --artifacts /path/to/work/latency-oracle-check
python3 experiments/latency/python/endpoint_wire_protocol.py
python3 experiments/latency/python/generate_fixture.py \
  --output-dir /path/to/work/synthetic-rd

latency-transform \
  --data /path/to/work/synthetic-rd/rd-data.bin \
  --metadata /path/to/work/synthetic-rd/rd-metadata.bin \
  --delta-us 150000 --profile stock-r3-hq-44100 \
  --output-dir /path/to/work/new-transform-result

# Use the existing private module configuration instead of --delta-us:
latency-transform \
  --data /path/to/work/synthetic-rd/rd-data.bin \
  --metadata /path/to/work/synthetic-rd/rd-metadata.bin \
  --config /data/adb/shizuka/config.conf --profile stock-r3-hq-44100 \
  --output-dir /path/to/work/new-config-transform-result

python3 experiments/latency/native/tests/compare_oracle.py \
  --binary /path/to/work/latency-build/latency-transform \
  --oracle experiments/latency/python/encoded_ttp_relay.py \
  --artifacts /path/to/work/new-native-comparison
```

The synthetic oracle checks 262,144 arithmetic cases and all nine packet
types with four wrapper roles. Native ASan/UBSan checks also cover 30,000
deterministic mutations, file boundaries, mapping bounds, all marker variants
and byte transparency. The cross-language comparison covers 121 valid cases
and 16 rejection cases. An Android ARM64 build has also transformed files on
the phone with matching output. Those are byte-format tests, not live RD/WR or
receiver latency tests.

## Work required for a live route

The actual `audio_pkt_dlkm.ko` uses a shared response queue across file
descriptors. Its file-release path unconditionally calls
`spf_core_apm_close_all` and `msm_audio_ion_crash_handler`. A separate process
opening GPR and then closing its descriptor can therefore interfere with the
existing HAL graphs, even when it sends only GET commands. **Standalone GPR
probes and a second audio-packet descriptor are prohibited for this route.**
The implementation must reuse the HAL's existing GPR registry and response
reader, dispatching only the relay's owned ports and tokens.

The private MAP probe received an accepted MAP handle. Its UNMAP acknowledgement
was consumed by the existing HAL reader and logged as an unknown client port
(`0x535a6201`); the probe did not receive a confirmed status. Remote cleanup
therefore remains unknown. This is not a successful MAP/UNMAP round trip, and
no retry or endpoint-data operation may rely on that handle. The probe never
opened a relay graph. The stock payload session is recovered through the
module's existing activation path, separately from these experiments.

The actual Adaptive graph has been identified, and the fresh-open GSL path is
being investigated. Its existing IMCL links must remain intact. The RD/WR OLC
client configuration also requires the container's offload-info state; adding
endpoint parameters to an ordinary container alone is insufficient. Static
firmware analysis identifies parent-container property `0x080010cb` as the
path that creates this state. Treating APSS as the parent and pacing both
disjoint paths inside the same container remain unvalidated. No relay graph
has been opened by this experiment.

The transport needs owned shared-memory mappings, cache synchronization,
token/epoch matching, bounded queues and confirmed RD/WR completion before a
buffer is reused. Offset zero is valid for the actual OSAL offset mapping and
must not be interpreted as a null physical address. A serialized command or
returned map handle alone does not establish safe lifetime or cleanup.

The owner must also prove module-ID/port ownership before altering a normal
fresh graph. An unsupported GET response or timeout does not prove a proposed
ID is unused. Size-query and blob-fill changes must use one provider revision;
failure after advertising a larger graph must abort that open operation.

For temporary root, the future relay belongs to the current module/root
session and must not require persistent root or an independent app root grant.
Opening KernelSU Action should only open the module-owned internal UI. A relay
owner must stop accepting new buffers on module disable/removal, root-session
loss, graph replacement or process exit, and resolve owned DSP buffers before
freeing them. A reboot invalidates the previous session; a later root/late-load
session needs fresh identity and ownership checks. This repository contains
the transform building block; these live ownership and cleanup mechanisms are
not enabled by the offline CLI.
