# GSL passthrough hook experiment

This is a **passthrough-only, disabled experiment**. The C wrapper calls the
original `acdb_ioctl` exactly once with its original five arguments and returns
its result. It adds no delay, transforms no payload, opens no DSP graph, and
provides no enable or namespace-override API. Every caller, including the three
future target call sites, currently passes through unchanged. The payload has
no imports, writable data, runtime relocations, provider reader, or transport.

The separate native relay graph/calibration components added later are not
linked into that124-byte passthrough payload. Their existing-node replacement,
56-byte size/fill transaction, immutable graph-epoch setting, ACK/profile gates
and scoped logger-calibration filter are specified in
[RELAY-INTEGRATION.md](RELAY-INTEGRATION.md). They require the embedding owner's
reviewed bootstrap/transport/lifecycle; they perform no device operation alone.

Only source files belong in this directory. Vendor libraries, firmware,
disassembly dumps, candidates, executables, and generated build evidence are
not included. Build and patch commands require an explicit output path outside
the checkout and reject an output path inside a detected Git checkout. Objects
and generated metadata go beside that output. Inputs are supplied separately.

## Exact target and gateway

The offline patcher accepts only stock `libar-gsl.so` SHA-256
`4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc`.
It verifies the `acdb_ioctl` PLT entry at RVA `0x38d50`, its GOT relocation at
`0x3c2f8`, and the original final `BR x17` at `0x38d5c`. ADRP/LDR/ADD and the
original dynamic linker/GOT resolution are preserved. Only that final branch
is replaced with a direct branch to the appended gateway.

The gateway preserves x0..x4 and adds `x5=x28` (graph), `x6=x30` (original caller
LR), and `x7=x17` (resolved original function). It tail-branches to the eight
argument C wrapper. The current wrapper is a leaf, leaves SP/LR/x18 and
callee-saved registers untouched, and uses `BR x16` to the original function.
Using x16 preserves the BTI call-compatible branch type; replacing it with a
generic `BR x7` would be wrong on a BTI-enforced mapping. The gateway and wrapper
start with BTI C; the target's existing BTI/PAC property note is preserved.
No return address is signed twice. The separately linked mock caller also
tests a normal PAC prologue/epilogue.

`shizuka_gsl_hook_classify` is a pure metadata function recognizing return RVAs
`0x246e4`, `0x247d0`, and `0x24e20` as size/driver/SPF-fill stages. It does not
enable transformation. `shizuka_gsl_hook_policy()` returns `0x00010000`:
ABI version1, provider enabled=false, namespace verified=false.

## ELF construction

The exact stock's last LOAD ends at `0x40587`; its occupied 16 KiB page ends at
`0x44000`. A read-only mapping at RVA `0x44000` holds the relocated program
header table. A separate RX mapping at `0x48000` holds the 124-byte payload.
Original virtual addresses, dynamic tables, Android packed relocations, RELR,
GOT slots, section contents, RELRO, and property notes remain intact.

The checker rejects overlapping LOAD pages, W+X, inconsistent file bounds,
wrong alignments, altered original bytes outside the named ELF-header fields
and one branch, or missing payload/PHDR mappings. Section headers are copied
and extended with `.shizuka.phdr`/`.shizuka.text`; the string and section tables
are relocated in non-loaded file data. Original build ID is retained and
therefore **does not identify the modified file**; use the candidate SHA-256.
Object-relocation auditing rejects absolute address literals/TLS/GOT imports
before the static link could conceal them. Final payload imports, runtime
relocations, and writable sections are also rejected.

The prototype's candidate SHA-256 was
`5935c37c0d0e61f224f25e618c46062955fb42b9eba750f08b167b38d81276bc`;
the freestanding payload ELF SHA-256 was
`09abb6571859e60f034fe80ee53b151875f3ca6f11775b6b6a21b43b097dfbdf`.
Neither candidate nor its bytes are a bundled deliverable. Android loading of
the candidate has not been tested.

## Reproduce without a device

Use Python with `pyelftools` and `unicorn`, the Android NDK, and an external work
directory. Set `SHIZUKA_NDK_BIN` to the NDK compiler directory,
`SHIZUKA_GSL_STOCK` to the separately supplied stock ELF, and
`SHIZUKA_GSL_WORK` to a work directory **outside the checkout**. From this source
directory:

```sh
python -B build_payload.py --output "$SHIZUKA_GSL_WORK/payload.elf"
python -B patch_gsl.py --stock "$SHIZUKA_GSL_STOCK" --payload "$SHIZUKA_GSL_WORK/payload.elf" --output "$SHIZUKA_GSL_WORK/libar-gsl.passthrough.so"
SHIZUKA_GSL_PAYLOAD="$SHIZUKA_GSL_WORK/payload.elf" python -B -m unittest -v test_gsl_hook.py
python -B build_mock.py --host --output "$SHIZUKA_GSL_WORK/payload-mock-host"
python -B build_mock.py --output "$SHIZUKA_GSL_WORK/payload-mock-arm64"
```

No command in these scripts uses ADB, loads a vendor library, mounts a file,
restarts a process, opens audio-pkt, calls GPR, or sends an audio command.
Unittest explicitly skips the vendor-fixture class if its external stock or
payload input is absent; a skip is not an ELF/ABI test pass.

Four offline tests passed with the actual stock: layout/original-byte
preservation, changed-stock/truncated-or-W+X-payload rejection, 64 randomized
actual-PLT -> gateway -> mock-original runs at two ASLR bases, and BTI tail-branch
plus policy/classification checks. They verify arguments, one original call,
return status/PC, x18/x19..x29/Q8..Q15/SP, and buffers, including an original
function's own response edit. Unicorn does not establish hardware BTI/PAC
enforcement or Android dynamic loader acceptance.

The host mock passed 512 cases. The pure Android mock passed 1024 cases on the
device (512 direct wrapper + 512 test gateway calls), zero failures, with
`gpr_opened:false`, `gsl_loaded:false`, and `graph_commands_sent:false`.
Its SHA-256 was
`a27a5db403ef91b65e3f3ba7d0655e2df184055399871d02e11ed8736e668770`.
It links only libc/libdl; no vendor library is loaded and the original function
is always the program's own mock. The device execution is an ABI checkpoint,
not evidence that the candidate GSL was loaded or latency applied.

## Boundaries for later work

A native SPF transformer, bounded external HQ/LL/Lossless provider, immutable
size/fill revision, fresh Adaptive session/mode guards, complete live namespace
ownership, and DSP SET/readback are still required. The wire-TTP and PCM-delay
approaches remain separate. No +150 ms runtime claim follows from this bundle.

Do not add a standalone GPR client as a shortcut. Investigation of the actual
audio-pkt driver showed a shared reader queue and close-time graph-cleanup
effects. Future transport must reuse the existing HAL GPR callback and driver
FD, without `gpr_init`/`gpr_deinit`, a second reader, or closing its FD. This
experiment intentionally contains none of those paths.

Private-copy GSL `dlopen` was not performed. GSL's own two initializers are CPU
feature detection, but all transitive dependency constructors/destructors have
not been audited. Only the pure, vendor-independent mock was device-executed.
