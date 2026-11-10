# APSS pre-OPEN Adaptive mode context

`src/latency_context.{h,cpp}` carries the private native HQ/LL/Lossless descriptor
through the existing Samsung AIDL byte vector. It does not change the signed DSP,
the negotiated first 11 bytes, or DSP parameters. In particular, the HQ minimum
buffer field is **not** a Lossless marker. Actual delay still requires the separate
fresh-graph/transport/profile gates and completed runtime buffer processing.

The native converter copies the 20-byte private proxy once, checks its selected
configuration, allocates 27 bytes, and appends this little-endian 16-byte tag:

| Offset | Value |
| --- | --- |
| 0 | `0x315a4853` (`SHZ1`) |
| 4 | private flags from the same proxy copy, as LE32 |
| 8 | selected sample rate, as LE32 |
| 12 | FNV-1a checksum of preceding 12 bytes followed by normalized fingerprint LE32 |

This is a consistency check on local APSS state, not a cryptographic authenticator.
The normalized fingerprint is FNV-1a (offset basis 2166136261, prime 16777619)
over the exact concatenation: ASCII `ACF1`; sample rate, channel mode, bits, HAL
mode as four LE32 values; buffering[6]; TTP[6] as their unsigned byte representations;
input mode and fade milliseconds as LE32; negotiated stream[11]. Padding,
pointers, logical vector length and capacity are excluded. This describes the
APSS codec configuration; it must not be compared directly to a differently
packed DSP INIT byte array without decoding that array first.

## Actual Samsung copy and strip boundaries

Addresses refer to the immutable DZIF inputs selected by the device profile.

* START: provider `A2dpOffloadAudioProvider::sehStartSession` retains the cfg in
  x22 (`0x1e738`), validates it (`0x1e768`, PLT `0x24a50`), and forwards that
  same object (`0x1e78c`) to base `sehStartSession`. The first owned codec copy is
  later at `0x108c4`; Adaptive vector copy `0x1aa78..0x1aaa8` uses `end-begin`.
  The libseh validator hook at `0xe988` captures and trims before this copy.
* UPDATE: provider `sehUpdateAudioConfiguration` does not invoke the validator.
  It copies at `0x118cc`, then calls libseh `ReportAudioConfigChanged` at
  `0x11a48`. The second hook at libseh `0x1bc7c` strips before the session copy
  (`0x1bda4`) and before forwarding the original input cfg to the callback
  (`0x1be8c..0x1be98`). The original function is called exactly once.
* A structurally readable 27-byte Adaptive vector is trimmed even when its tag
  fails, so a rejected APSS suffix cannot reach the original UPDATE consumer.
  Invalid tags invalidate attestation. START rejects the invalid tagged input.
* The Samsung AIDL parcel reader uses a variable byte array (`0x1ee20` in
  `vendor.samsung.hardware.bluetooth.audio-V2-ndk.so`). Its writer uses only
  `end-begin` (`0x1ef1c..0x1ef2c`), so a subsequent copy/parcel contains 11 bytes.
* Actual provider Adaptive destruction at `0x1a514..0x1a524` frees `begin` via
  unsized delete. Trimming `end` to 11 while retaining capacity 27 is safe.

The supplied binaries show Samsung provider as the only external importer of the
Samsung Session start/report entry points. No external `UpdateAudioConfig`
importer was found. This evidence is specific to the exact profiled binaries.

## Reader contract

`src/latency_context.h` is a C/C++ compatible 64-byte POD layout. The hidden
`s25_latency_context` lives in the appended HAL writable segment, and its RVA
must be taken from the **matching final** `hal-payload.elf`, rather than copied
from a previous build. No pointer in the published record requires relocation.

Read sequence with acquire ordering; reject zero or odd. Read all fields, apply
an acquire fence, then read sequence again. Accept only identical nonzero even
sequences, magic `0x31585a53`, ABI 1, size 64, and valid 1. A graph freezes this
sequence and its fields before OPEN; a later sequence or mismatched accepted INIT
must revoke its profile gate. A matching tag preserves Lossless as mode 12288
although PAL/DSP still receives HQ mode 4096. Private flags and sample rate must
match that mode and the normalized configuration.

Legacy 11-byte inputs remain accepted by the codec validator but invalidate mode
attestation. Repeated validation can retain one previously observed tagged
allocation only when cfg, begin, capacity, normalized fingerprint, the untouched
16-byte tail, and current published sequence all match. It never reads a spare
capacity tail from an unobserved legacy allocation, and invalidation discards this
cache. This cache does not constitute a general allocator-lifetime tracking API;
new private Adaptive configurations must continue to enter with the full tag.

## Offline verification

The `latency_context` host case includes start/update exact-once forwarding,
HQ/LL/Lossless, corrupt-tag rejection and removal, legacy invalidation, unchanged
11-byte copies, repeated validation, and concurrent seqlock snapshots under
ASan/UBSan. The backend's native build includes the new source in both payloads,
adds the second HAL hook, and rejects runtime relocations/W+X mappings.

After a normal native build, `tests/latency_context_emulation_check.py` executes
both actual patched ARM64 entry points with synthetic cfg/stack memory. It stops
before original vendor bodies and verifies the two original prologues, register
preservation, stack replay, tag stripping and published fingerprint/mode for
12 cases. It performs no dlopen, GPR, driver, Bluetooth, or device operation.
