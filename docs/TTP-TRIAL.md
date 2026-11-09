# Adaptive HQ / Lossless / Adaptive LL TTP experiment

The [shared control API](CONTROLS.md) can now generate a versioned plan for this
builder. [Opt-in diagnostics](DIAGNOSTICS.md) provides a path for collecting the
existing target/TTP/state logs. Saved intent does not install or hot-apply a
candidate.

Status on 2026-11-09: **the DZIF DSP loader rejects the modified image during
signature verification**. The +150 ms candidate did not execute and has no
successful listening or latency evidence. The original DSP was restored.
These DSP images are not included in the normal KernelSU module. New DZIF
trial requests are refused before mounting, with the signature limitation as
the reason. Arithmetic candidates remain available for offline analysis.

The tuning API retains saved integer intent and plan generation, but reports
`runtime_apply_supported: false`. `config.sh tuning availability` exposes the
reason. A saved value of 150 with `applied: false` is not an active +150 ms
setting. Do not call this an audio-quality fix.

## Confirmed loader rejection and recovery

The allocated-section +150 ms / LL0 candidate has SHA-256
`cdb67b33ae62e67e4d3cb160870c2c24eaa860fb750426ae8b2e41ca872f0646`.
In capture session `db6abca9-72d0-4529-8f8d-cb7c2fcce60d`, taken after confirming
the Air6 Pro connection and codec context, the raw DSP ring repeatedly reported:

```text
ADSP:[AS]: Streaming Hash Finalize error
ADSP:[AS]: Segment [0] failed to read or streamed SigVerify for aptx_adaptive_enc_module.so.1
ADSP:[AS]: Error: dlopen_ex failed for aptx_adaptive_enc_module.so.1 (flags 2)
```

The local investigation retains the raw capture as
`retry-sections-raw.jsonl`. This is a loader rejection before the injected
instructions run. Correct ELF section metadata and passing instruction tests
do not establish that a signed DSP image can be replaced. Earlier failed trials
also produced host graph-open errors and speaker output; those observations
alone did not identify the cause. An earlier accounting-observer trial using
the same original DSP image had also recorded streamed signature rejection.

Recovery restored the original DSP SHA-256
`05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3`.
The restored native/APK/HAL payload hashes match the previously heard 0.9.2
stock-DSP generation, so its exact-payload proof can retain that normal
session. That proof does not cover any +150 ms DSP candidate. No signing or
verification bypass is implemented.

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

## Host configuration limits on the signed image

There is an accepted HQ minimum request, but it is not an arbitrary additional
TTP control. `AdaptiveConfig.buffering[2]` is passed through the Samsung offload
configuration and `lib_bt_aptx.so`'s `aptx_ad_pack_enc_config`
(`0x6a8c..0x6be8`) into DSP INIT parameter `0x08001183` (59-byte payload).
In the exact DSP image, `aptx_adaptive3_enc_set_param` at
`0xfd1c..0xfd70` reads payload `+0x14` into `ctx+0x42b0`; at
`0xfdd0..0xfde4`, the internal HQ maximum (`ctx+0x42b4`, default 200 ms)
clamps that minimum. `calcTTPAdj` at `0x14f14..0x14f8c` uses the resulting
minimum in its floor calculation, with a further 10 ms term. This can affect
the existing policy floor; it cannot supply an independent +150 ms on top of
the observed roughly 187..202 ms adjustments. It also does not prove a larger
receiver buffer or better audio.

The inspected INIT explicitly ignores the LL minimum/maximum requests.
PROFILE parameter `0x08001184` is deprecated; successful host serialization
does not make its timing fields effective. The configuration-file callback
does not accept a TTP target. The host latency-mode API selects modes rather
than an integer millisecond offset. No supported host path for arbitrary,
independent HQ/Lossless/Adaptive-LL increments has been established on DZIF.

The HQ target words are both 70: legacy initialization writes at
`0x25c8` / `0x25d0`, and R3 initialization at `0xb1d0` / `0xb1e0`.
Do not infer a target of 94 from an adjacent register assignment: stores in
the same Hexagon packet use the old value without `.new`.

## Offline candidate behavior

The experiment adds independent bounded increments to calculated Adaptive HQ
and Adaptive LL adjustments. Bitrate/frame-size tables and encoder modes remain
unchanged in the offline model. Saved controls accept integer HQ 0..150 ms /
LL 0..100 ms (step 1). Domains and modeled contexts are declared in
`config/tuning.json`, not embedded in the common planner.

| Selection | Arithmetic effect in the offline candidate |
| --- | --- |
| `--delta-ms 0` | Byte-identical stock control image |
| `--delta-ms 50` | Add 50,000 us in HQ states 3 / 7 |
| `--delta-ms 100` | Add 100,000 us in HQ states 3 / 7 |
| `--ll-delta-ms 23` | Add 23,000 us in Adaptive LL states 2 / 6; HQ remains stock unless requested |
| Both increments zero | Byte-identical stock image |
| Other states | Preserve the stock result |

The candidate hook targets shared `calcTTPAdj` after the stock clamps and the
lossless-capable floor, before the existing diagnostic log and 8-us packing.
This intentionally shifts the scheduling adjustment beyond the stock policy
range for the trial; it does **not** raise an internal target word or establish
a larger receiver buffer. Adaptive HQ (including 96 kHz) and Lossless HQ share
this path; actual state 3/7 must be confirmed for each tested stream. Adaptive
LL uses the independent state 2/6 path. Adding LL scheduling margin increases
latency in the model without selecting HQ. Loader rejection prevents this from
being a runtime claim. Separate aptX LL and LC3 are different encoders.

The guard includes the subsequent state term (`ctx+0x6494 * 1000 us`) when
checking the unsigned 16-bit wire limit of 524,280 us. If the addition cannot fit,
or the incoming adjustment is already outside that range, it leaves the stock
result untouched. It never wraps an added increment into a short delay. Other
codecs and the separate LE Audio encoder are not patched.

The two original debug branches remain intact. Their diagnostic becomes
`Shizuka TTP HQ requested +50ms; calculated us ...` (or `+100ms`). The number is
the post-guard adjustment before the final state term; “requested” is deliberate:
a state outside the requested mode or ceiling rejection does not receive the
increment. With an LL request the label becomes
`Shizuka TTP HQ+50 LL+23 requested; calculated us ...`; the decoder exports the
HQ and LL requests separately.

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

Use another output directory for every request. Direct canonical
`--set adaptive_hq_ttp_delta_ms=37 --set adaptive_ll_ttp_delta_ms=23` and a shared
API `--plan` are also supported. Both zero requires no LLVM tools and produces
the stock control. Nonzero candidates have an instruction harness.
The builder refuses unknown hashes, occupied padding, changed hook packets,
unexpected symbols/ELF layout and existing output directories. Its manifest
records all modified ranges, input/output hashes and explicit unverified state.
The verifier reproduces the patch and rejects altered candidates/manifests.

The harness executes the **actual hook and bias bytes extracted from the
candidate**, at their linked addresses. Mode-specific ceiling vectors check
known/unknown states, both original debug branches, ordinary values, signed/large
inputs, exact ceiling boundaries and final state terms. It also checks the
preserved registers and return. The Linux exit stub follows the
[QEMU Hexagon test startup convention](https://github.com/qemu/qemu/blob/master/tests/tcg/hexagon/crt.S).
This verifies instruction behavior, not the DSP loader, receiver or audio.

## Runtime boundary

The former private one-shot DSP mounting path is disabled for new DZIF trial
requests because the signed loader rejected the candidate. Existing experiment
recovery remains available so rollback can unmount a previously active trial,
restart the DSP loader and rebuild the stock audio graph. Saving tuning intent,
normal boot and standard activation do not stage DSP images.

Further work must first establish an accepted configuration path. Repeating
the same image modifications at 0/50/100/150 ms cannot resolve signature
verification. If an accepted path is found, fresh target/state/TTP measurements
and receiver or acoustic timing are still needed to assess actual delay, pops
and audio quality. QHS observations and prior stock listening are separate
evidence. Cold reboot → DFRoot → KernelSU also remains a separate open check.

## 0.9.1 offline validation

On 2026-11-09, 210 candidates were built on the Mac: every integer 0..100
individually for HQ and Adaptive LL, the 0/50/100 matrix, and additional mixed
values. Their actual instruction bytes passed 473,088 vectors in a Linux QEMU
VM running locally on the same Mac. Checks cover state separation, both debug
branches, signed/large inputs, exact ceilings, final state terms and preserved
registers. All eight prior non-stock 0/50/100 image hashes reproduce exactly.
This is firmware instruction/ABI evidence; the later device trial failed loader
acceptance, so it adds no audible result. The standard module ships no DSP trial.

Common dispatch and schema compatibility are in `tools/shizuka/ttp.py`.
Addresses, image gates and instruction generation are isolated in
`tools/shizuka/dsp_backends/samsung_dzif_ttp.py`; the independent instruction
oracle is in `samsung_dzif_verify.py`. Another firmware must register its own
verified implementation. The shared planner and persistence need no new device
branch to expose another provider or integer control.

## Historical +150 ms candidate and activation design (0.9.2)

HQ/Lossless accepts 0..150 ms; Adaptive LL remains 0..100 ms. The +150/LL0
candidate is SHA-256
`ed7ab35d40bfe15e1fa09d08467ac8257f78b149afe41f50fe5620cc9a389dd1`.
Another 100 candidates (HQ 101..150 with LL0/100) passed 234,336 actual-byte
vectors on the Mac-local VM. This validates arithmetic/ABI, not audible quality.

Version 0.9.2 introduced private, root-owned one-shot staging, an owned mount
journal and timed rollback. Those checks established file publication and
recovery, not DSP acceptance. The later allocated-section candidate retained
the arithmetic but still failed streamed signature verification. The current
backend refuses a new request before a mount is created; the former staging
format is not an available runtime application API.
