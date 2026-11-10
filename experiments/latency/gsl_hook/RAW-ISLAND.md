# Default-off RAW-island experiment

`SHIZUKA_RELAY_RAW_ISLAND` defaults to `0`. Only an explicit source build with
`-DSHIZUKA_RELAY_RAW_ISLAND=1` selects this graph alternative. It is an
experimental topology, not an established continuous-forwarding fix. The
embedding runtime/builder must also identify the selected mode; a setting value
or the ordinary production zero-delta bypass must not enable it implicitly.

The zero-delta device differential reproduced the same RD2/WR1 timeout as the
positive-delta graph, while the recorded upstream-RT property delivered to WR
was type1/value1. These observations exclude TTP mutation and missing RT as
necessary causes of that observed stall. They do not establish its cause.

## Exact graph change

All modules remain in the same fresh subgraph `b00000ab` and ADSP domain2.
Both container IDs and every module instance ID already belong to the original
graph; no new namespace, GPR client, or signed-DSP modification is introduced.

| Container | Current relay | RAW-island alternative |
| --- | --- | --- |
| E5 `e00000e5` | SAL43f3 → logger43f4 → splitter43f5 → RAT43f6 | Same modules, followed by converter43f8 → encoder43fb → RD43f7 |
| E6 `e00000e6` | Converter43f8 → encoder43fb → RD43f7, plus WR43fa | WR43fa only |

WR43fa still connects to connproxy4098 in the original external container
E83/SG6c. The existing splitter43f5:3 →44e9 branch stays intact. The module
connection records are otherwise identical to the current relay transform:
the old PCM logger edge is removed, converter connects directly to encoder,
and encoder connects to RD. Original data-port properties are preserved except
the already reviewed RD/WR logger substitutions.

Both existing `080010cb` parent fields change from `ffffffff` to the same owned
APSS client535a6101. The same RD/WR client parameters and RD one-frame parameter
are appended. No property is duplicated. The total SPF growth remains56B;
the E5 module group grows24B and E6 shrinks24B inside the existing module-list
parameter. Original parameter order, padding, IMCL payloads and unrelated graph
records remain intact.

The alternative requires an exact complete original contract: the eight stock
MID/MIID pairs, their original E5/E6 membership, all eight data-port shapes,
the eight stock intra-SG connections, both complete seven-property GC
configurations, ADSP domain2, stack4096, normal heaps and unowned parent fields.
Malformed, previously transformed, foreign-container, missing-node, duplicate,
or unexpected-parent inputs fail before output is written. The normal build
continues to produce its original byte-for-byte graph output.

## Capacity hypothesis and primary evidence

Source: AudioReach engine commit
`5773bf8e627c9857481dd338a9ec4aea4a49afd3`. Actual firmware SHA256:
`de7ec2ab21308fbce5f5c87c845fd2995e8367f3a39978c6bde63573969dd3b6`.

The source `gen_cntr_buf_util.c:1135,1184` stores a container-wide PCM threshold
byte value; line415 assigns that value to an unthresholded RAW branch. Lines
1301–1379 cover unmarked parallel branches. The actual implementations match:
LCM bytes are cached at `b0258b40..50` and replaced with PCM values at
`b0258c1c..40`; RAW threshold0/1 takes the global byte value at
`b0258edc..ee4`, then writes the new port threshold at `b0258ff4`.

The source `cu_ext_port_util.c:33..46` hardcodes two peer buffers for RAW data,
before the ordinary RT/single-buffer policy. Actual `b024ee7c..eed8` does the
same. Consequently, WR_DONE can mean that the first664B input was copied into
two downstream buffers; it does not prove connproxy consumed a complete R3
frame. A hypothetical352B branch threshold could accommodate that first input
in704B total while leaving the following input blocked. The final capacity and
this causal explanation have not been observed directly.

With E6 containing only the RAW WR source, no PCM threshold module remains to
contribute that container-wide value. The default RAW threshold is2048B in
`gen_cntr_get_default_port_threshold` (`gen_cntr_buf_util.c:120`; actual
`b0592a74`). This motivates the bounded experiment. Dynamic acceptance and
capacity/continuity remain runtime proof obligations.

## Ownership, calibration and IMCL

The exact source ACDB SHA256 is
`142204cf70b76ca48089b9e9abf2fcec775e08f02b634005827906ad991b35a1`.
Across its1042 distinct graph blocks, E5/E6 and the eight involved MIIDs occur
only in SG00ab, in36 variants. Both original containers have identical seven
properties. Saved ACDB absence alone is not ownership proof: the existing
fresh-SG/source-registration/open-reference/runtime-context gates still apply.

The SG's36B driver property block contains routing2, flush0 and SG type2; it
does not bind modules to container IDs. `gsl_acdb_parse_sg_drv_prop_data`
(`gsl_graph.c:643..701`) reads those SG properties. Tagged-module results contain
MID/MIID, and processor selection remains ADSP2.

`apm_gpr_cmd_parser.c:1240..1276` creates the module DB record with the host
container selected by the submitted module list. The same file routes later
module properties at1539, module calibration at3172 and IMCL at2444 through
that current host-container pointer. All original module MID/MIID/SG identities
and all IMCL bytes remain unchanged. The existing scoped nonpersistent logger
calibration filter still removes only reviewed logger parameters for43f7/43fa;
RAT, converter, encoder and unrelated calibration payloads remain identical.
No calibration value is used as a substitute for a missing topology proof.

## RAT and startup limits

GC supports RD external outputs in a signal-triggered container. The actual RD
OLC client configuration at `b0260438..0568` has no STM rejection and selects
the same READ-queue setup function `b0261940` used by ordinary RD. It also sets
single-buffer policy on all E5 external inputs, which is a broader buffering
effect than changing the RD port alone.

RAT and encoder configuration stays unchanged. The PCM converter API expressly
cannot resample (`pcm_converter_api.h:40..45`), so the original RAT→converter→
encoder44.1kHz route already has matching rates. Equal44-sample quanta remain
compatible through a bit-width conversion; this experiment does not force a
different RAT duration or encoder threshold.

Signal-driven RD does not gain an immediate data trigger merely from READ
arrival. On a RAT tick, missing external output buffers can cause stale RAW
output and metadata to be dropped (`gen_cntr_data_handler_island.c:449..464` →
`gen_cntr_data_handler.c:14..46`). The initial trial deliberately keeps the
current startup policy and two READ buffers. It must observe first-buffer
validity and sustained continuity; it must not infer no drops from OPEN/START
acceptance. A future pre-START READ barrier would require its own reviewed
queue/lifecycle proof.

## Offline checks

`test_relay_graph.py` builds both flag values, checks the normal byte oracle,
the exact alternative byte oracle, malformed/foreign contracts and unchanged
IMCL/nonstructural payloads. `raw_island_graph_test.c` exercises both container
orders, exact-sized truncated allocations and single-bit mutations under
ASan/UBSan, plus the existing size/fill/ACK epoch and calibration mechanisms.
The normal test registration is in `latency_runtime_native_test.py`.

Private offline evidence for all36 actual ACDB graph variants is stored under
`work/latency-research/raw-island-review/`; no vendor graph or audio payload is
checked into this source bundle. Runtime acceptance and continuous playback
must be reported separately from these offline results.
