# HQ / Lossless TTP experiment

The [shared control API](CONTROLS.md) can now generate a versioned plan for this
builder. [Opt-in diagnostics](DIAGNOSTICS.md) provides a path for collecting the
existing target/TTP/state logs once device validation is available. Saved intent
still does not install or hot-apply a candidate.

Status: offline candidates only. Nothing is mounted automatically, and these
DSP images are **not included in the KernelSU module**. The user deferred
listening on 2026-11-09. The connected phone still uses its unmodified DSP image;
its snapshot had no connected audio device, so fresh three-mode baselines could
not be collected. Loader acceptance, live TTP, receiver delay, poor-link pops and
long playback remain unverified. Do not call this an audio-quality fix.

## What the exact phone image establishes

The phone's `/vendor/dsp/adsp/aptx_adaptive_enc_module.so.1` matched SHA-256
`05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3`.
This image differs from the handover's address interpretation:

- `0x21fa8` contains a diagnostic string, not a host-populated latency table.
- `setEncoderCfgVals`' PC-relative HQ table is the image's read-only
  `bitRateLevelMapTableHQ` at `0x22718`. Its first fields are bitrate, a float
  frame duration and sample counts. They do not populate `ctx+0x124`.
- HQ timing words at `ctx+0x12c` / `ctx+0x130` initialize to 70 in both legacy
  and R3 initialization. `calcTTPAdj` multiplies the selected word by 4,000 us.
  The 70 therefore contributes 280 ms **before** overhead, pending PCM and
  clamps; it is not a measured end-to-end delay. Hexagon packet stores use the
  old register value unless `.new` is present.
- `ctx+0x124` is a separate word used by the LL timing/servo path. Patching it
  from the handover would not reliably change HQ.

No usable host-side configuration for those HQ words was established. The
inspected `aptx_adaptive_enc.cfg` callback handles `global_settings` /
`override_quality`, not an HQ latency target. Incoming PROFILE timing fields are
deprecated/ignored on this build; increasing maxHQ alone is ineffective.

## Trial behavior

Rather than change bitrate/frame-size tables or mislabel a host property, this
experiment adds a bounded increment to the actual calculated HQ adjustment.

| Selection | Effect |
| --- | --- |
| `--delta-ms 0` | Byte-identical stock control image |
| `--delta-ms 50` | Add 50,000 us in HQ states 3 / 7 |
| `--delta-ms 100` | Add 100,000 us in HQ states 3 / 7 |
| LL / other states | Preserve the stock result |

The hook runs in shared `calcTTPAdj` after the stock clamps and the
lossless-capable floor, before the existing diagnostic log and 8-us packing.
This intentionally shifts the scheduling adjustment beyond the stock policy
range for the trial; it does **not** raise an internal target word or establish
a larger receiver buffer. Adaptive HQ (including 96 kHz) and Lossless HQ share
this path; actual state 3/7 must be confirmed for each tested stream.

The guard includes the subsequent state term (`ctx+0x6494 * 1000 us`) when
checking the unsigned 16-bit wire limit of 524,280 us. If the addition cannot fit,
or the incoming adjustment is already outside that range, it leaves the stock
result untouched. It never wraps an added increment into a short delay. Other
codecs and the separate LE Audio encoder are not patched.

The two original debug branches remain intact. Their diagnostic becomes
`Shizuka TTP HQ requested +50ms; calculated us ...` (or `+100ms`). The number is
the post-guard adjustment before the final state term; “requested” is deliberate:
an LL invocation or ceiling rejection does not receive the increment.

## Build and verify

Requirements: project Python dependencies, LLVM `llvm-mc` with Hexagon v73
support and `ld.lld`. No stock binaries are committed to the repository.

```sh
python tools/ttp_trial.py inspect --input /path/to/aptx_adaptive_enc_module.so.1
python tools/ttp_trial.py build --input /path/to/aptx_adaptive_enc_module.so.1 \
  --delta-ms 50 --output /new/path/ttp-50 \
  --llvm-mc /path/to/llvm-mc --linker /path/to/ld.lld
python tools/verify_ttp_trial.py --input /path/to/aptx_adaptive_enc_module.so.1 \
  --candidate /new/path/ttp-50 --output /new/path/ttp-50/harness.elf \
  --llvm-mc /path/to/llvm-mc --linker /path/to/ld.lld
# On Linux (the verifier's --qemu option can also execute it):
qemu-hexagon /new/path/ttp-50/harness.elf
```

Repeat with a different output directory for 100. A 0 build requires no LLVM
tools and produces the stock control; the instruction harness is for 50/100.
The builder refuses unknown hashes, occupied padding, changed hook packets,
unexpected symbols/ELF layout and existing output directories. Its manifest
records all modified ranges, input/output hashes and explicit unverified state.
The verifier reproduces the patch and rejects altered candidates/manifests.

The harness executes the **actual hook and bias bytes extracted from the
candidate**, at their linked addresses. For each increment, 1,920 vectors check
known/unknown states, both original debug branches, ordinary values, signed/large
inputs, exact ceiling boundaries and final state terms. It also checks the
preserved registers and return. The Linux exit stub follows the
[QEMU Hexagon test startup convention](https://github.com/qemu/qemu/blob/master/tests/tcg/hexagon/crt.S).
This verifies instruction behavior, not the DSP loader, receiver or audio.

## Device experiment still required

Before the first mount, collect fresh HQ 44.1 kHz, HQ 96 kHz/24-bit and Lossless
44.1 kHz baselines. Record the actual state, target word, final state term and
calculated adjustment with units. Compiled defaults or old captures must not be
reported as these measurements. A fresh diagnostic target-word observation is
still needed; this candidate's log alone does not provide it.

Any later activation must recheck the exact firmware and live DSP hash, close
the old encoder graph, and use an owned, reversible bind mount in the loader's
mount namespace. Do not write a partition, stack onto an unknown mount, change
SELinux policy, reuse old listening proof or make this a boot default. A bind
mount by itself does not replace a DSP image already loaded in a running graph.
Verify a new graph's diagnostic marker and calculated TTP after loading, and
restore the stock file **and a fresh stock graph** after each timed trial.
The regular Shizuka rollback does not manage a separate experimental DSP mount;
this offline tool deliberately does not create one.

Trial order is stock → +50 → stock → +100 → stock, on the same music and device
settings. Record HQ44.1 and Lossless44.1 pop count/period and long playback,
including a reproducible poor-link condition. Use measured receiver PCM/playout
delay or a repeatable acoustic impulse correlation to show whether receiver
buffering responds. The phone-side arithmetic or an already stable radio link
cannot establish this. QHS is kept as its existing, separately verified opt-in;
this experiment does not force encoder bitrate/quality or prove a downstream
QHS effect. Cold reboot → DFRoot → KernelSU remains a separate open check.
