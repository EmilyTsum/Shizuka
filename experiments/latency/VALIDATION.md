# Validation record — 2026-11-10

This record distinguishes earlier component checks from live audio behavior.
The experiment is excluded from ordinary module packages and is available
through explicit optional packaging. Subsequent controlled in-HAL development
trials have confirmed attach, owned OPEN, default calibration, MAP,
event-registration and encoder INIT acknowledgements with the signed DSP
unchanged. Completed RD/WR forwarding, an applied positive TTP change and
receiver latency remain unconfirmed. The earlier results below are historical
component evidence, not a final validation of the changing integrated source.

| Component | Result and scope |
| --- | --- |
| C++ transform + configuration | Two CTest suites passed under host ASan/UBSan. Transform checks include 262,144 arithmetic boundaries, 108 packet-type/role/marker cases and 30,000 deterministic mutations. Configuration checks cover typed ranges, canonical integers, unrelated module choices, private ownership/modes, symlinks, FIFO refusal, missing/oversized input and atomic replacement. |
| Independent oracle | Frozen Python reference passed 262,144 arithmetic cases and 36 packet-type/role cases. Native comparison passed 121 valid and 16 rejection cases, with identical data, forwarded metadata and 44-byte physical WR command bodies. Offset-zero serialization is checked against explicit wire words. |
| Endpoint wire format | Helper self-test and six additional test groups passed, including exact lengths, returned status, address/handle ownership, capacities, flags, frame counts and signed timestamp boundaries. No packets were sent by these tests. |
| Android configuration/file transform | ARM64 CLI read the actual root-owned module configuration with HQ=150 ms and transformed synthetic file input: primary 197,000→347,000 us; secondary zero remained zero. All other encoded/forwarded bytes and the signed timestamp were preserved. The configuration was not modified. |
| GSL offline candidate | Four ELF/Unicorn tests passed against the exact privately supplied stock ELF: original-byte/layout preservation, invalid input rejection, 64 randomized gateway calls at two ASLR bases, and policy/BTI branch checks. Candidate was not loaded into Android. |
| Pure GSL mock | Host 512 cases and phone ARM64 1,024 cases passed. The mock loads no vendor library and opens no GPR device. It proves mock-call ABI behavior, not real GSL loading, hardware enforcement or graph behavior. |

The Android configuration/file-transform binary SHA-256 was
`23fb4573ed4fdcefed435d0c6b0a52463a7cb4c104146d1e7b2cf6c53f7b523b`.
The pure ARM64 GSL mock SHA-256 was
`a27a5db403ef91b65e3f3ba7d0655e2df184055399871d02e11ed8736e668770`.
Executables, stock libraries, firmware and private captures are not bundled.

The earlier standalone GPR MAP/UNMAP attempt did **not** pass. MAP returned a
handle; UNMAP status was not observed because the existing HAL consumed its
response. Actual driver analysis additionally established shared queues and
global graph/ION cleanup on descriptor release. All such probes were stopped;
they must not be reused as diagnostic tools. The ordinary module session was
then restored with the exact previously heard payloads. Final module status
verified the active native/APK/HAL hashes, original signed DSP and disarmed
watchdog. This restoration check does not supply new audible proof.

The [transport evidence](transport/TRANSPORT-PROOF.md) and
[lifecycle contract](TRANSPORT-LIFECYCLE.md) distinguish implemented HAL-owned
communication, root-authorized configuration delivery and owner guards from
the remaining validation of complete OLC parent semantics, cache/buffer
lifetimes, encoded-session forwarding and receiver timing. Settings changes
remain pending until a fresh owned graph epoch. No final full-source evidence
has been recorded for the ongoing integration work.
