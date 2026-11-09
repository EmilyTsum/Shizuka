# Codec control API

The KernelSU screen is provisional. Its future One UI design can call the same
APIs; it does not own a second preference store or Lossless classifier.

`CodecTuning` is the shared internal Java model. `BtConfig` exposes it on Android
and `CodecControlTool` exposes it on a host JDK. Responses are versioned JSON.
There is no network listener.

## Ownership and application

Android owns codec selection, sample rate, selection bit depth, transport and
profile permissions. Existing `config.sh device ...` operations use Android's
store. No connect-time codec replay is introduced.

Shizuka owns additional controls with a verified implementation. Saving tuning
returns `applied:false`. A plan states its provider, context, activation method
and scope. DSP observations remain separate evidence; a successful save is not
an applied acknowledgement.

| Control | Values | Current scope |
| --- | --- | --- |
| `aptx_hq_ttp_delta_ms` | 0, 50, 100 ms | Exact DZIF Adaptive HQ and Lossless HQ together |
| `adaptive.hq.ttp_delta_ms` | Alias of the same key | Not an independent Adaptive setting |
| `lossless.hq.ttp_delta_ms` | Alias of the same key | Not an independent Lossless setting |

The current DSP hook sees shared HQ states 3/7. It cannot independently filter
Android's Adaptive/Lossless item or sample rate, so capabilities expose that
coupling. LL delay, separate aptX LL, LC3 buffer controls and receiver buffer
depth are unsupported. Their plans return `state:unsupported`; a nonzero HQ
intent does not leak into those plans.

Activation is `offline_dsp_build_then_fresh_graph`, not hot apply. Standard
KernelSU Action does not apply this saved TTP value. See [TTP-TRIAL.md](TTP-TRIAL.md).

## Android API

Run in a root shell:

```sh
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning capabilities
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning get
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning set adaptive.hq.ttp_delta_ms 50
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning plan samsung-sm-s931z-dzif adaptive hq 96000 24
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning plan samsung-sm-s931z-dzif lossless hq 44100 16
```

The context explicitly identifies codec/mode/rate/selection-depth. Lossless
requires HQ / 44100 / 16; its DSP input remains 24 bit. Adaptive HQ needs a
concrete supported rate and 24-bit context.

The existing private `ExtensionConfigStore` serializes internal and CLI writers
with a stable lock and atomic replacement. The new key defaults to zero.
Existing QHS trust/migration rules are retained. `accepted_change` identifies
the write; `requested` is the current snapshot, which another writer can change.
Neither is playback evidence.

## Host planning and candidate builds

```sh
python tools/codec_control.py capabilities
python tools/codec_control.py plan --config config/config.example.conf \
  --codec adaptive --mode hq --rate 96000 --bits 24 > plan.json
python tools/ttp_trial.py build --plan plan.json \
  --input /path/to/aptx_adaptive_enc_module.so.1 --output /new/path/candidate \
  --llvm-mc /path/to/llvm-mc --linker /path/to/ld.lld
```

The host uses the same Java model as Android. The builder validates schema,
provider, types, context, coupling and activation before compiling, and stores
the plan in the candidate manifest. The instruction verifier checks it again.
`--plan` and `--delta-ms` are mutually exclusive; existing `--delta-ms 0|50|100`
remains available. A candidate build never installs anything.

## Extension points

Add verified controls/providers in `CodecTuning`, with typed values, supported
contexts and an explicit activation boundary. Independent HQ/Lossless or
per-rate tuning needs a backend that can actually discriminate those contexts.
Keep persistence in the shared store and Android-owned preferences in
`DevicePreferences`.

Add observed fields with units/source/freshness once in `DspLogDecoder`.
Capture, replay, JSON export and the provisional display then share their
interpretation. Unrecognized messages remain in raw logs. Future UI controls
render capabilities and unsupported state instead of inventing implied support.
