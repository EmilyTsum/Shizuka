# Encoded TTP transform component

This is an offline C++17 component and file CLI. It does not open a graph,
allocate APM memory, modify a signed DSP image, change a codec, decode/re-encode
audio, or send anything to a controller or receiver.

`validateAndRewrite(RDBuffer, delta_us, Profile)` requires a complete successful
OLC RD result. Its caller must positively identify the stock R3 / 44.1 kHz / HQ
profile. A plausible header cannot identify a codec. LL, Lossless, legacy
versions and other rates are rejected by the explicit profile contract.

The delta is a runtime `uint64_t` argument, copied once for the entire RD buffer.
Only timing-bearing R3 wrapper TTP words change, in exact 8-us units. A primary
197,000-us header plus 150,000 us becomes 347,000 us. Split-TX secondary zero
sentinels remain zero. Unset primary TTP with a positive delta and unexpected
nonzero secondary TTP are rejected. Values are checked before narrowing;
overflow never wraps or clamps.

`readDelayConfig(path)` reads an immutable snapshot of the same canonical
schema-2 configuration saved by the module UI. For this reviewed HQ profile,
`adaptive_hq_ttp_delta_ms=150` becomes `150000` us without rebuilding any
binary. Each file invocation reads one snapshot. A future root-side provider
can read a new requested snapshot without changing an in-flight buffer. The
initial live transport must accept settings at a fresh graph epoch and use
that immutable value for every buffer; safe midstream updates require separate
receiver/timeline validation. Saving a new value alone cannot attest that it
has become active.
The separate LL setting is parsed but does not authorize an LL transform.
Missing files, invalid schemas, unknown/duplicate keys, aliases and out-of-range
values reject rather than retaining an earlier positive delta. Use the module
configuration CLI to migrate older formats. The file and its immediate parent
must be regular/private (0600/0700), owned by the effective UID. The reader uses
`openat` on the checked directory and refuses final symlinks and special files.
The reviewed ranges mirror `config/tuning.json`: HQ 0–150 ms, LL 0–100 ms.

The parser accepts contiguous proven wrapper boundaries only; it never scans or
resynchronizes. Every wrapper and metadata record passes validation before a
separate output buffer is produced. Encoded bytes outside each two-byte TTP
word are identical. Bluetooth metadata records retain their exact wire bytes,
including offsets and flags. Exactly one client-owned frame-info record must
cover the full RD data; it is consumed into the unchanged timestamp bits and
WR end-of-frame/timestamp-valid flags. Unknown IDs, ownership/tracking tokens,
extensions, reserved flags, truncated records and non-4-byte payloads reject the
whole buffer. The allowlist is deliberately limited to `0x0A001037`,
`0x0A001038`, `0x0A001039` and client frame-info `0x0A001052`.

The transform is bounded to 4 MiB of encoded data and 64 KiB of metadata.

The public wire contracts are pinned to AudioReach commit
`5773bf8e627c9857481dd338a9ec4aea4a49afd3`:

- [Metadata layout and flags](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/modules/metadata_api.h)
- [Client encoder frame info](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/modules/codec_metadata_api.h)
- [WR command body](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/modules/wr_sh_mem_ep_api.h)
- [Physical and offset memory maps](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/apm/apm_memmap_api.h)
- [Bluetooth metadata](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/interfaces/module/metadata/api/bt_metadata.h)

## WR command serialization

`serializeWriteCommand` emits the 44-byte
`DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2` body only. `Mapping` explicitly selects
`AddressMode::Physical` or `AddressMode::Offset` and carries a declared capacity.

- Physical mode requires a nonzero 64-byte-aligned address and a declared span
  within the 36-bit address space. The data length must fit that span.
- Offset mode accepts zero as the first buffer offset. The offset must be
  64-byte aligned and the complete buffer must fit one actual mapped region.
  That region's capacity must be a positive 4-KiB-aligned uint32 value. Full
  64-bit values are checked before serialization; high bits are never discarded.
- A nonempty buffer requires a positive existing APM map handle. Empty metadata
  uses zero address, handle and capacity, producing zero metadata command words.

The owner must supply these fields from its owned mapping record, match returned
RD/DONE handles and offsets, synchronize caches and manage buffer lifetimes.
Neither a CLI argument nor successful serialization proves mapping ownership,
validity, routing or receiver behavior. The stock GSL/OSAL offset path must not
be treated as a physical-address path.

## Host validation

Build outside the repository, for example:

```sh
cmake -S experiments/latency/native -B /path/to/work/latency-build \
  -DSHIZUKA_LATENCY_SANITIZERS=ON
cmake --build /path/to/work/latency-build
ctest --test-dir /path/to/work/latency-build --output-on-failure
```

The native tests check every uint16 TTP value for four deltas, all nine packet
types with four roles and three markers, signed timestamp bit boundaries,
mapping bounds including offset zero, whole-buffer rejection and 30,000
deterministic malformed/mutation inputs under ASan/UBSan.

Compare generated synthetic fixtures with the independent Python prototype:

```sh
python3 experiments/latency/native/tests/compare_oracle.py \
  --binary /path/to/work/latency-build/latency-transform \
  --oracle /path/to/encoded_ttp_relay.py \
  --artifacts /path/to/work/new-comparison-directory
```

The reviewed oracle SHA-256 is
`fa98734c0c79d0f31bdf642f52aa5da3ffa6413d20a541dd69ae148a972dcdaf`.
Its physical-mode 44-byte serializer is compared byte for byte. Offset-zero
serialization is checked against the explicit public command-word layout.
The comparison also checks rejected inputs, no-overwrite output and symlink/
oversize file refusal. All fixtures it generates are synthetic. No private
capture or extracted firmware belongs in this directory.

## Offline CLI

```sh
latency-transform --data rd-data.bin --metadata rd-metadata.bin \
  --delta-us 150000 --profile stock-r3-hq-44100 \
  --output-dir new-result-directory
```

Instead of `--delta-us`, use `--config /data/adb/shizuka/config.conf` to read
the UI's HQ value. These options are mutually exclusive. This is still a file
transform; it does not connect the setting to live Bluetooth.

The parent directory must exist and the destination must not exist. Input files
must be bounded regular files; final-component symlinks are refused. The CLI
validates first, writes private files in a private sibling staging directory,
then publishes the complete directory using an atomic no-replace rename. It
never overwrites an existing destination. Output is `wr-data.bin`,
`wr-metadata.bin` and a scope-explicit `relay-report.json`.

The optional format-only command body additionally requires all seven options:

```sh
  --address-mode offset \
  --data-address 0 --data-map-handle 7 --data-capacity 4096 \
  --metadata-address 64 --metadata-map-handle 8 --metadata-capacity 4096
```

Those values illustrate a declared mapping, not an actual live allocation. The
CLI writes `wr-command.bin`; it still performs no graph or device I/O.

Android ARM64 builds use the NDK's `aarch64-linux-android34-clang++` and
`-static-libstdc++` with the same C++ sources. They need only Android's libc,
libm and libdl at runtime. No output binary is committed.
