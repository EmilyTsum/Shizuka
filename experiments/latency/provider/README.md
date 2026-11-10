# Root-side configuration lease publisher

This component reads the existing private module configuration as root and
publishes a bounded binary lease for the implemented in-HAL reader. It never opens
GPR, an audio device, a DSP graph or a socket. The HAL does not read the
root-private configuration. This publisher is experimental and excluded from
ordinary module packages; the optional runtime package includes it with a
separate source-bound identity. Publishing a lease alone does not enable a graph
or establish completed encoded forwarding.

## Authority and lifetime

The executable requires effective UID 0 and effective `CAP_SYS_ADMIN`. It pins
the canonical `/data/adb/modules/s25_aptx_adaptive` directory, its root-private
state directory and active marker. It checks the module ID, active state,
absence of `disable`/`remove`, saved `hal.pid`/`hal.start`, actual HAL PID/start
ticks, its own PID/start ticks and boot UUID. A changed directory, replaced
activation marker, HAL restart, root loss or invalid configuration stops it.

The publisher reads `/data/adb/shizuka/config.conf` using the shared native
`readDelayConfig` validator: schema 2, canonical keys and values, owner-only
0700 directory/0600 regular file, no final symlinks or special files, bounded
read and modification checks. No stale positive value survives a failed read.
HQ 0–150 ms and LL 0–100 ms are converted to integer microseconds. Publishing
LL intent does not authorize an LL transform.

Every 500 ms it rechecks authority and atomically replaces a lease expiring
two seconds after its `CLOCK_MONOTONIC` timestamp. Revision starts at 1 and
increments when HQ or LL values change; heartbeat refresh alone retains the
revision. The cookie is the nonzero 16-byte source-bound runtime build identity,
supplied as 32 lowercase hex characters from the verified optional package.
Boot and owner process identities provide the additional session binding. The
reader must compare the expected cookie,
boot and HAL identity, validate ownership/format/CRC and reject expiry. CRC is
an integrity check; root-owned filesystem permissions establish the authority
boundary.

The in-HAL graph owner freezes the accepted value at graph-epoch creation.
Later published revisions remain pending for the next session until receiver
timeline behavior is validated. A valid publisher lease alone does not grant
permission to create or change a DSP graph.

`SIGTERM`, `SIGINT`, `SIGHUP` and detected failure stop publication and unlink
only the file inode last published by this owner. Failed unlink is reported;
the reader must still enforce expiry and owner-process identity. The publisher
never deletes a replacement file belonging to another owner. `SIGKILL` or a
crash cannot run cleanup, so the two-second lease is mandatory. The process
holds a nonblocking exclusive lock to prevent concurrent publishers.

## File boundary and wire layout

Output is fixed at `/data/vendor/audio/shizuka/provider.bin`. The directory is
root:`audio` mode 0750; records are root:`audio` mode 0640, regular and singly
linked. The `audio` group is resolved by name. Existing unsafe files or
directory replacement are rejected. A new private temporary file is written,
fsynced, renamed within the pinned directory and followed by directory fsync.
An open reader sees one complete old or new record.

The deployment owner must verify the actual HAL's group access and SELinux
label before using this path. This code does not change SELinux policy, make
the configuration public or grant the HAL root. `provider.lock` is mode 0600.

All integers are little-endian. The record is exactly 128 bytes:

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `SHZLAT1` followed by NUL |
| 8 | 4 | Version 1 |
| 12 | 4 | Record size 128 |
| 16 | 16 | Raw boot UUID bytes in displayed hexadecimal order |
| 32 | 16 | Activation cookie bytes |
| 48 | 8 | Configuration revision |
| 56 | 8 | Expiry in monotonic nanoseconds |
| 64 | 4 | Requested HQ microseconds |
| 68 | 4 | Requested LL microseconds |
| 72 | 4 | HAL PID |
| 76 | 4 | Publisher PID |
| 80 | 8 | HAL `/proc/PID/stat` start ticks |
| 88 | 8 | Publisher start ticks |
| 96 | 28 | Zero reserved bytes |
| 124 | 4 | CRC-32/IEEE of bytes 0–123, reflected polynomial `0xedb88320`, initial/final XOR `0xffffffff` |

## Build and checks

Run from the repository root. Generated binaries must be outside the checkout:

```sh
python3 experiments/latency/provider/build_provider.py --host \
  --output /path/to/work/provider-host
python3 experiments/latency/provider/build_provider.py \
  --ndk-bin /path/to/ndk/toolchains/llvm/prebuilt/host/bin \
  --output /path/to/work/shizuka-provider
```

Host mode builds and runs ASan/UBSan tests covering the exact layout, CRC,
corruption, bounds, UUIDs, capability/process parsing, atomic replacement,
permissions, lock exclusion, symlink/FIFO rejection and owned-inode cleanup.
It uses the test user's UID/GID only in the filesystem library; the executable
has no flag that relaxes the Android root requirement. Android mode builds an
ARM64 API-34 executable with static C++ runtime and 16-KiB load alignment.

The intended explicit invocation, after deployment/label checks, is:

```sh
/data/adb/modules/s25_aptx_adaptive/latency-provider \
  --module-dir /data/adb/modules/s25_aptx_adaptive \
  --output-dir /data/vendor/audio/shizuka \
  --cookie 0123456789abcdef0123456789abcdef --hal-pid HAL_PID
```

The cookie above is illustrative and must be replaced with the exact packaged
runtime build cookie.
There is no daemonization, automatic root acquisition or module installation
hook in this executable. The module's explicit lifecycle starts it for the
current HAL owner. Root-loss recovery and end-to-end HAL consumption remain
under device validation; lease publication is not RD/WR or audible proof.

## Read-only runtime status

The native module's availability API may invoke the hash-verified bundled
latency-provider with --status and the canonical --module-dir. This mode never
publishes or unlinks a lease. It returns one JSON object and exit status 0,
including an explicit unavailable reason when files, authority or observations
are absent.

It reads the 128-byte root-owned provider lease and the 256-byte
/data/vendor/audio/shizuka/status.bin record. The latter must be a singly linked
audioserver-owned, audio-group mode-0640 regular file in the existing root:audio
mode-0750 directory. Reads are bounded, refuse final symlinks/special files and
reject concurrent modifications. SHZSTAT1 version 1, size 256, zero reserved
bytes 232–251, boolean fields and CRC-32/IEEE over bytes 0–251 must validate.
The little-endian uint64 at offset 224 counts rewritten wrappers whose WR_DONE
completion was confirmed; transforming a pending buffer does not increment it.

Boot, activation cookie, HAL and provider identities must match. Actual HAL and
publisher start ticks, the module session and the publisher's root capability
are checked through /proc. Status age and remaining lease must both be within
two monotonic seconds. Missing, malformed, stale or mismatched records cannot
report an applied delay.

An applied packet change additionally requires an owned nonzero graph epoch,
completed WR buffers with matching RD count, reviewed R3/HQ/44.1-kHz context,
the exact Adaptive SG/container, accepted INIT/OPEN/START, worker/registration
readiness, owned mappings, ACTIVE phase 5, no error, and a bounded before/after
TTP difference equal to the accepted delta. Positive deltas also require a
nonzero WR_DONE-confirmed rewrite count. Owned mappings and in-flight buffers
are normal during ACTIVE; their counts are not cleanup-error indicators.
Pause, drain and uncertainty cannot report an active change. Bootstrap counters alone are not an
active transform. A later provider revision may leave the old graph active
while a changed HQ request is pending the next session. LL intent remains
separate and unverified.

The JSON includes observation age and lease time remaining. The native UI
expires its cached applied indication against both limits and displays saved
requests separately from the observed graph, packet TTP, completed buffers and
debug details. Completed encoded buffers do not establish measured receiver
latency or bit-perfect Lossless transmission.

## Read-only health and owner recovery

`--health [--module-dir PATH]` reads the same bounded records and returns a
JSON `healthy` result with exit0 for healthy or exit2 for unavailable/invalid.
It retains the current module, root-capability, boot, HAL and publisher checks,
and additionally matches the lease cookie against the root-private
`state/latency-runtime.active` record and the saved publisher PID/start ticks.
Three immediate status-read attempts accommodate a concurrent worker write;
they do not extend either two-second freshness deadline. There is no missing
status grace beyond the lifecycle's existing one-second publisher startup wait.

Health does not require an applied delay or any RD/WR frames. READY, PAUSED,
ordinary DRAINING and CLOSED are healthy when the lease/status are fresh, the
worker remains alive and no error is reported. Live mappings or buffers alone
are normal ownership. Expiry, stale or mismatched identity, worker exit,
nonzero error, UNCERTAIN or an unknown phase fails health. The status ABI has
no separate need-close flag; the runtime must publish a nonzero error for a
fault that requires owner recovery, including quarantined ownership.

The module watcher invokes the hash-verified `latency-runtime.sh health` entry
only while a latency activation marker exists. A failed check joins its
existing generation-token-bound `control.sh rollback-health` recovery path.
The health check itself never signals a process, releases a buffer, closes a
graph, unmounts a payload or mutates audio. The ordinary root owner performs
recovery and retains unresolved ownership until its existing teardown can
confirm cleanup. Stale watcher tokens and system shutdown do not initiate a
new recovery.

`--recovery-check --module-dir PATH --cookie HEX32 --hal-pid PID --hal-start TICKS`
is a separate read-only cleanup check. It does not require a surviving publisher
or lease file. It still requires the canonical root-owned module/session,
current boot and cookie, the original still-live HAL PID/start, and a fresh
audioserver-owned CRC-checked status record. The HAL must report lease revoked.
A changed PID, expired status or missing record cannot discharge DSP ownership.

The only no-SSR results are an untouched owner (epoch/source/open/INIT/start,
RD/WR/rewrite counters, mappings, in-flight buffers and error all zero), or an
owned CLOSED stage with exact target CLOSE acknowledgment and zero remaining
mappings, buffers and error. The source port is published at the first owned
ADVERTISE before that caller returns. Global GSL traffic counters need not be
zero. The legacy `last_ack_status` field holds the acknowledged command opcode;
target CLOSE is `0x01001004`.

The lifecycle writes the private `state/latency-remote-uncertain` journal
atomically and syncs it **before** stopping a relay publisher. Cleanup is checked
only after quiescing and removing the GSL overlay; bounded polling permits late
CLOSE/unmap completion and lease revocation. An earlier clean classification is
rechecked rather than trusted across a later graph change.
Otherwise the reviewed Samsung owner stops its normal audio HAL, requests one
normal ADSP loader restart and requires observed offline then running states.
It never calls the separate crash entry or restarts thermal services. The
journal remains until a fresh normal HAL, mapped stock GSL, stock DSP and all
core/effect/Bluetooth factory binders agree. Timeouts retain the ownership
records and require diagnosis instead of repeated reset requests.

A canonical journal from a different boot cannot trigger SSR or signals to old
PID numbers. After stock DSP/GSL and a fresh full normal HAL are verified in the
new boot, old ownership is archived with its old boot identity, with the fence
retired last. Stale request archives additionally include the cookie so
successive boots preserve separate histories; archive collisions are refused.
