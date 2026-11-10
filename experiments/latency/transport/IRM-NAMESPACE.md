# IRM report GET does not establish a free module namespace

`0x08001210` is `PARAM_ID_IRM_REPORT_METRICS`. It reports the currently
configured/enabled profiling metrics. It is not an inventory of registered
APM/GPR module IDs or module handles. A missing MIID therefore does not prove
that the ID is available for relay endpoints.

No device request was sent for this audit. The reviewed actual GET also writes
synthetic counters into existing metric payloads before copying the report,
so it must not be treated as a side-effect-free diagnostic.

## Public contract

AudioReach commit `5773bf8e627c9857481dd338a9ec4aea4a49afd3`:

- [irm_api.h:644–721](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/modules/irm/api/irm_api.h#L644)
  identifies the parameter and describes its block/instance/metric report.
  Each block carries an instance ID and metric count; it does not carry a
  namespace-completeness claim or a reservation token.
- [irm_cmd_handler.c:769–848](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/modules/irm/src/irm_cmd_handler.c#L769)
  checks the caller's capacity against the existing report size, traverses the
  IRM block/instance/metric lists, writes incrementing test values beginning
  at `0x1000`, then copies the existing report. A small capacity yields
  `AR_ENEEDMORE`; it does not request every module from APM.
- [irm_cmd_handler.c:1716–1887](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/modules/irm/src/irm_cmd_handler.c#L1716)
  creates that report from the configured IRM lists. A disabled/unprofiled
  instance can be absent while still existing in APM.
- [irm_apm_if_utils.c:250–315](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/modules/irm/src/irm_apm_if_utils.c#L250)
  separately implements an internal APM handle request used by enable-all
  profiling. This is not the `0x08001210` external GET contract. Enabling a
  profiler to populate the report would change runtime state and still would
  not reserve IDs atomically against a concurrent graph open.

## Actual firmware correspondence

The analyzed sectioned ELF SHA-256 is
`de7ec2ab21308fbce5f5c87c845fd2995e8367f3a39978c6bde63573969dd3b6`.
The local extracted GET disassembly SHA-256 is
`8f9695f44a63de08a710908719e7530d7a07afe579f17255e0d9f31ac7c31134`.
The proprietary binary and disassembly are not committed here.

| Address | Observed operation |
| --- | --- |
| `0xb016b638`, `0xb016b6dc` | Select parameter `0x08001210` and branch to its handler. |
| `0xb016b800–804` | Compare `IRM+0x6c` report size with the supplied capacity; insufficient capacity goes to the shared need-more result. |
| `0xb016b808–830` | Traverse `IRM+0x70` and nested list/object pointers: block, instance, metric. |
| `0xb016b810` | Initialize a counter to `0x1000`. |
| `0xb016b848–874` | Follow the metric's payload pointer, store the counter into the existing payload and increment it for configured profile samples. |
| `0xb016b898–8ac` | Copy from `IRM+0x68` for `IRM+0x6c` bytes and set the parameter's returned size/status. |

The actual offsets and control flow match the public report implementation,
including its synthetic-value writes. The GET does not walk the APM module
database or GPR registration table and does not request all handles. A zero-size
or empty report only establishes that this IRM report has no configured data.

## Consequence for the relay

Do not use this GET as an absence probe, and do not enable profiling to make it
look like a complete inventory. The new endpoint IDs still need an ownership
and allocation mechanism tied to the HAL's normal fresh graph lifecycle.
That mechanism must also prevent a concurrent open from acquiring the same ID.
The global namespace guard remains unresolved by this IRM parameter.
