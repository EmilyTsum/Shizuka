# DSP diagnostics

0.9.0 is built and host-tested, but its acquisition path has not been run on a
phone. Device FARF acceptance, permissions, playback overhead and the cold
DFRoot/KernelSU chain remain unverified. Capture is opt-in; status refresh and
boot do not start logging. The KernelSU display is provisional.

## Data path

```text
stock DSP FARF -> stock adspmsgd ring, read-only
 -> dsp-capture JSON Lines -> shared DspLogDecoder
 -> raw.jsonl + sampled events.jsonl + summary.json
 -> private atomic telemetry.json -> status.sh / provisional display
```

Status, control and diagnostics share one DEX library. The existing
`policy.dex`/`status.dex` names remain identical/hash checked for compatibility.
The small ARM64 reader has independent source/output build records.

## Capture and export

Run in a root shell, with the verified module active:

```sh
sh /data/adb/modules/s25_aptx_adaptive/diagnostics.sh capture 60
sh /data/adb/modules/s25_aptx_adaptive/diagnostics.sh list
sh /data/adb/modules/s25_aptx_adaptive/diagnostics.sh export session-UUID
sh /data/adb/modules/s25_aptx_adaptive/diagnostics.sh recover
```

Capture accepts 5..120 seconds and streams JSON Lines. Private session files
live under `/data/adb/shizuka/diagnostics/session-UUID/`:

| File | Meaning |
| --- | --- |
| `raw.jsonl` | Original DSP messages and native capture/end records |
| `events.jsonl` | Decoded observations sampled at most four times per second |
| `summary.json` | Final fields, boot/process identity, Android selection and exit status |
| `native.stderr` | Acquisition, refusal and cleanup details |

Export returns a JSON `archive` path for a completed capture. Only these four
files are included. Traversal, symlinks and unsafe ownership are refused.
Transfer explicitly, for example:

```sh
adb exec-out su -c 'cat /data/adb/shizuka/diagnostics/RETURNED-ARCHIVE.zip' > capture.zip
```

Raw data is capped at 16 MiB; export is capped at 32 MiB total. Data remains
root-private. There are no automatic uploads, broad logcat dumps, audio-sample
capture or inclusion of runtime logs in normal module packages.

## Observations and scope

Fields include packet/state/quality/interval/size/channels, DSP sample rate,
system/latency state, logged HQ target and calculated TTP/trial request,
QHS/scan/coexistence/audio flags, received IMCL quality/RSSI/environment feedback,
and logged `ENABLE_*`/`SIMULATE_FORCE_LOSSLESS` flags. Each has a value, unit,
source, time, age and freshness. Missing values are absent/null. Unrecognized
messages remain in raw logs; unlogged internal variables are not inferred or
read through arbitrary memory/register access.

`encoder_lossless:true` requires the explicit fresh
`STATE_ENCLEVEL_5_LOSSLESS` packet state. Android's selection, quality 5, support
bits and debug/force flags do not set it. Unknown states yield null. Receiver
bit-perfect transmission remains unverified.

Summary QHS, received QHS support and CQDDR preferred-QHS are distinct. Received
RSSI is distinct from the R3 summary raw-RSSI field. HQ target words retain their
units. Calculated TTP precedes the final state term/wire packing; it is not
measured receiver delay or occupancy.

Times are APSS `CLOCK_MONOTONIC` observations with ring/polling delay, not DSP
emission/presentation times. Counter resets, initialization, rate changes,
clock reversal and changed Android selection/route reset the decoder epoch.
A full unobserved ring wrap cannot be counted from this ABI.

Live status requires the current boot, Bluetooth/HAL PID **and start ticks**,
fresh DSP rate, Adaptive/Lossless selection and a single matching A2DP context.
Ended, stale, differently routed and ambiguous observations are hidden.
Dynamic freshness is three seconds and is recalculated by status/display/copy
consumers. The logged sample rate is epoch-scoped, since legacy can report it
only at setup; it remains valid only with a live matching session, fresh DSP
observations and no epoch reset. Its original observation age is retained.
Android selection is sampled separately and can lead DSP reconfiguration;
process/rate correlation is not a radio/receiver acknowledgement.

## Acquisition and cleanup

The first backend covers exact Samsung DZIF Adaptive legacy/R3. Separate aptX
LL and LC3 encoders need their own verified capture/decoder providers. Adaptive's
own LL mode/state can be observed without claiming an LL delay control.

The wrapper accepts known stock/+50/+100 DSP hashes and the built reader hash.
Existing FARF overrides are refused. The collector publishes one complete owned
hard link using mask 4 and a fixed encoder/feedback filename list. It changes no
quality override, codec, DSP binary, property or audio route and restarts no
service.

A unique new 262144-byte ring is duplicated through pidfds, identity checked,
mapped `PROT_READ` and accessed with read-only DMA synchronization. Ambiguous
buffers, unsupported permissions or ABI mismatch fail closed.

A detached watchdog is armed before publication. On owner death/ADB disconnect
or deadline, it waits for the sole publisher to exit and removes only its exact
owned inode, preserving replacements. Staging names encode boot/PID/start ticks.
Startup recovery or explicit `recover` handles verified stale owned files and
leaves a current owner alone. A kernel/power interruption can require recovery
when root/module support is next available; cold boot remains untested.

## Offline replay

```sh
python tools/codec_control.py replay raw.jsonl --format native > observations.jsonl
python tools/codec_control.py replay old-ring.txt --format ring > observations.jsonl
```

Replay uses the same Java decoder, marks data `replay:true`, `live:false`,
`available:false`, and never publishes phone telemetry. Future probes/backends
need exact-image/ABI validation while retaining explicit source, units,
freshness and scope.
