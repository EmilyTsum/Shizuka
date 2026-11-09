# Codec control API

The KernelSU screen is provisional. A future One UI screen can use the same
capabilities, settings and observations. There is no network listener.

## Shared definitions and ownership

`config/tuning.json` is the source of truth for canonical names, compatibility
aliases, integer type, unit, default, minimum, maximum and step. It also declares
which providers support each device/firmware, codec, mode, sample rate and bit
depth. `TuningCatalog` is device-independent; `CodecTuning` plans matching
operations. Multiple controls can match one context. Neither class contains a
Samsung profile check, DSP address or aptX-specific mode rule.

`tools/generate_tuning.py` emits `TuningDefinitions.java` from that catalog.
Android builds, host planning and tests reject stale generated definitions.
Catalog changes also invalidate source-bound build and test evidence. Android
`BtConfig` and host `CodecControlTool` use the same Java planner. The Python DSP
builder independently checks the public JSON contract against the same catalog.

Android owns codec selection, sample rate, selection bit depth, transport and
profile permissions. `config.sh device ...` uses Android's store. Tuning does not
replay codec preferences when a device connects.

## Current providers

| Canonical key | Integer domain | Current implementation |
| --- | --- | --- |
| `adaptive_hq_ttp_delta_ms` | 0..100 ms, step 1, default 0 | DZIF Adaptive HQ and Lossless HQ together; DSP states 3/7 |
| `adaptive_ll_ttp_delta_ms` | 0..100 ms, step 1, default 0 | DZIF Adaptive LL; DSP states 2/6, independent of HQ |

`aptx_hq_ttp_delta_ms`, `adaptive.hq.ttp_delta_ms` and
`lossless.hq.ttp_delta_ms` remain compatibility inputs for the HQ key.
`adaptive.ll.ttp_delta_ms` aliases the LL key. New responses and saved files use
canonical `adaptive_` names. Supplying the same control twice, including through
an alias, is rejected even if both values agree. Reading an old file is passive;
a subsequent save/migration writes the canonical spelling. QHS trust rules are
preserved, including during alias migration and concurrent saves.

Adaptive LL playback was already supported. This version adds its **additional
TTP adjustment** API. Separate aptX LL playback is also distinct from Adaptive
LL; its additional TTP adjustment has no provider here. Neither do LC3 receiver
buffers or independent Adaptive/Lossless HQ increments. Unknown profiles and
unsupported contexts return `state:unsupported`, with no fallback to S25 offsets.

A positive LL adjustment adds scheduling margin and latency while retaining the
LL encoder mode. It does not prove better audio quality, larger receiver buffers
or a negotiated Lossless state. HQ/Lossless remain coupled by the actual DSP ABI.

## Android API

Run in a root shell:

```sh
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning capabilities
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning get
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning set adaptive_hq_ttp_delta_ms 100
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning set adaptive_ll_ttp_delta_ms 23
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning plan samsung-sm-s931z-dzif adaptive ll 48000 24
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning plan samsung-sm-s931z-dzif lossless hq 44100 16
```

Responses use schema 2. A context plan contains canonical whole-profile
`parameters`, matching `operations` (provider/backend/key/value/unit/activation/
coupled contexts), and context-scoped state. One image can contain both HQ and
LL requests even when the selected context's request is zero. `applied:false`
always distinguishes saved intent/planning from runtime evidence.

The current activation method is `offline_dsp_build_then_fresh_graph`.
Saving tuning or running standard KernelSU Action does not install a DSP
candidate or hot-apply a value. `ExtensionConfigStore` retains private paths,
stable locking and atomic replacement. `accepted_change` identifies a write;
`requested` is the current snapshot, which concurrent writers can change.

## Host planning and candidate builds

```sh
python tools/codec_control.py capabilities
python tools/codec_control.py plan --config config/config.example.conf \
  --profile samsung-sm-s931z-dzif --codec adaptive --mode ll --rate 48000 --bits 24 > plan.json
python tools/ttp_trial.py build --plan plan.json \
  --input /path/to/aptx_adaptive_enc_module.so.1 --output /new/path/candidate \
  --llvm-mc /path/to/llvm-mc --linker /path/to/ld.lld
```

The builder checks schema, types, limits, context, provider, coupling and
activation. It records the plan in the manifest; the instruction verifier checks
it again. Old schema-1 plans remain readable through a boundary adapter. New
output is schema 2. `--plan` cannot be mixed with direct parameters. Direct
`--set adaptive_hq_ttp_delta_ms=37 --set adaptive_ll_ttp_delta_ms=23` uses the same
canonical definitions. `--delta-ms`/`--hq-delta-ms` and `--ll-delta-ms` remain
compatibility flags. Candidate generation never writes to a device.

## Adding a device or control

1. Declare a typed control/domain and profile/provider contexts in
   `config/tuning.json`; regenerate definitions. Existing controls can be reused.
2. Add/reuse a reviewed DSP backend under `tools/shizuka/dsp_backends/` and
   register it in the dispatcher. Stock hashes, offsets, symbols, instruction
   generation, mode-state mapping and the independent execution oracle belong
   there. JSON cannot import code or run a command.
3. Verify the actual candidate bytes and firmware conditions. Merely adding a
   profile name/hash does not establish binary compatibility. Supply a runtime
   profile's optional `profile_diagnostics_preflight` for its acquisition ABI.
4. Keep persistence in the shared store and Android preferences in
   `DevicePreferences`. Add observed fields once in `DspLogDecoder`, with source,
   unit and freshness. Capture, replay, export and UI then share interpretation.

The regression suite uses another device, codec, rate, bit depth, signed domain
and unit, with two controls in one context, to verify these extension boundaries.
See [PORTING.md](PORTING.md), [TTP-TRIAL.md](TTP-TRIAL.md) and
[DIAGNOSTICS.md](DIAGNOSTICS.md).
