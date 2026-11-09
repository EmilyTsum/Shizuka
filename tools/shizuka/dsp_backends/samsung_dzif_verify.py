# SPDX-License-Identifier: GPL-2.0-only
"""Independent DZIF Hexagon instruction/ABI oracle, executed on candidate bytes."""

import subprocess
import tempfile
from pathlib import Path

from .samsung_dzif_ttp import (
    CAVE,
    HOOK,
    MAX_WIRE_US,
    compile_patch,
    patch_verified,
    plan_increments,
    sha256,
)


def vectors(delta_ms, ll_delta_ms=0):
    # An independent wire-field oracle; exercise both existing debug branches,
    # unknown states, signed/large inputs and the final extra state term.
    candidates = {
        0,
        32000,
        60000,
        179000,
        187000,
        197000,
        210000,
        MAX_WIRE_US,
        MAX_WIRE_US + 1,
        (1 << 32) - 1,
        (1 << 64) - 1,
    }
    for increment in {delta_ms, ll_delta_ms} if ll_delta_ms else {delta_ms}:
        for extra in (0, 6, 50):
            limit = MAX_WIRE_US - increment * 1000 - extra * 1000
            candidates.update((limit - 1, limit, limit + 1))
    result = []
    for state in (*range(10), 255, 0xFFFFFFFF):
        for candidate in sorted(candidates):
            for extra in (0, 6, 50, 0xFFFFFFFF):
                for debug in (0, 1):
                    wanted = candidate
                    increment = (
                        delta_ms if state in (3, 7) else ll_delta_ms if state in (2, 6) else 0
                    )
                    total = candidate + increment * 1000 + extra * 1000
                    if increment and total <= MAX_WIRE_US:
                        wanted += increment * 1000
                    result.append(
                        (
                            state,
                            candidate & 0xFFFFFFFF,
                            candidate >> 32,
                            extra,
                            debug,
                            wanted & 0xFFFFFFFF,
                            wanted >> 32,
                        )
                    )
    return result


HARNESS = """
.section .resume0,"ax",@progbits
{ r8 = #0 }
{ jumpr r31 }
.section .resume1,"ax",@progbits
{ r8 = #1 }
{ jumpr r31 }
.section .start,"ax",@progbits
.global _start
_start:
{ r26 = ##cases }
{ r27 = ##cases_end }
{ r16 = ##context }
{ r18 = ##debug_word+0x24 }
{ r19 = ##parameters }
.Lcase:
{ r2 = memw(r26+#0) }
{ memw(r19+#0) = r2 }
{ r20 = memw(r26+#4); r21 = memw(r26+#8) }
{ r2 = memw(r26+#12) }
{ memw(r16+##0x6494) = r2 }
{ r2 = memw(r26+#16) }
{ memw(r18+##-0x24) = r2 }
{ r0 = ##0x12345678; r1 = #99 }
{ r17 = #1000 }
{ r22 = #22; r23 = #23 }
{ r24 = #24; r25 = #25 }
{ call s25_ttp_hook }
{ r9 = memw(r26+#20); r10 = memw(r26+#24) }
{ p0 = cmp.eq(r20,r9); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r21,r10); if (!p0.new) jump:nt .Lfail }
{ r9 = memw(r26+#16) }
{ p0 = cmp.eq(r2,r9); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r8,r9); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r0,##0x12345678); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r1,#99); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r17,#1000); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r22,#22); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r23,#23); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r24,#24); if (!p0.new) jump:nt .Lfail }
{ p0 = cmp.eq(r25,#25); if (!p0.new) jump:nt .Lfail }
{ r9 = ##context }
{ p0 = cmp.eq(r16,r9); if (!p0.new) jump:nt .Lfail }
{ r9 = ##debug_word+0x24 }
{ p0 = cmp.eq(r18,r9); if (!p0.new) jump:nt .Lfail }
{ r9 = ##parameters }
{ p0 = cmp.eq(r19,r9); if (!p0.new) jump:nt .Lfail }
{ r26 = add(r26,#28) }
{ p0 = cmp.eq(r26,r27); if (!p0.new) jump:nt .Lcase }
{ r0 = #0 }
{ jump .Lexit }
.Lfail:
{ r0 = #1 }
.Lexit:
{ r6 = #94 }
{ trap0(#1) }
.section .cases,"a",@progbits
.balign 8
cases:
@VECTORS@
cases_end:
.section .context,"aw",@nobits
.balign 8
context: .space 0x6500
parameters: .space 8
debug_word: .space 8
"""
HARNESS_LINKER = """ENTRY(_start)
s25_ttp_hook = 0x14fb4;
SECTIONS {
 .hook 0x14fb4 : { *(.hook) }
 .resume0 0x14fc0 : { *(.resume0) }
 .resume1 0x14fdc : { *(.resume1) }
 .bias 0x23a30 : { *(.bias) }
 .start 0x30000 : { *(.start) }
 .cases 0x40000 : { *(.cases) }
 .context ALIGN(0x10000) : { *(.context) }
 /DISCARD/ : { *(.text) *(.comment) *(.note*) }
}
"""


def build_harness(candidate, manifest, stock, destination, llvm_mc, linker):
    ll_delta_ms = manifest.get("adaptive_ll_delta_ms", 0)
    pieces = compile_patch(manifest["delta_ms"], llvm_mc, linker, ll_delta_ms)
    expected, expected_manifest = patch_verified(
        stock, manifest["delta_ms"], pieces, ll_delta_ms, manifest.get("layout", "legacy_padding")
    )
    if "control_plan" in manifest:
        if plan_increments(manifest["control_plan"]) != (manifest["delta_ms"], ll_delta_ms):
            raise ValueError("TTP increment differs from the control plan")
        expected_manifest["control_plan"] = manifest["control_plan"]
    if candidate != expected or manifest != expected_manifest:
        raise ValueError("Candidate/manifest is not the reproducible exact-stock patch")
    cases = vectors(manifest["delta_ms"], ll_delta_ms)
    with tempfile.TemporaryDirectory(prefix="shizuka-ttp-check-") as directory:
        root = Path(directory)
        source = root / "harness.s"
        # Execute bytes extracted from the candidate, not a freshly assembled
        # behavioral substitute for the hook/bias being validated.
        (root / "hook.bin").write_bytes(candidate[HOOK : HOOK + 12])
        (root / "bias.bin").write_bytes(candidate[CAVE : manifest["rx_end"]])
        prefix = (
            '.section .hook,"ax",@progbits\n.incbin "hook.bin"\n'
            '.section .bias,"ax",@progbits\n.incbin "bias.bin"\n'
        )
        rows = "\n".join(".word " + ",".join(map(str, row)) for row in cases)
        source.write_text(prefix + HARNESS.replace("@VECTORS@", rows))
        (root / "harness.ld").write_text(HARNESS_LINKER)
        subprocess.run(
            [
                str(llvm_mc),
                "-triple=hexagon",
                "-mcpu=hexagonv73",
                "-filetype=obj",
                "harness.s",
                "-o",
                "harness.o",
            ],
            cwd=root,
            check=True,
        )
        subprocess.run(
            [str(linker), "--no-relax", "-T", "harness.ld", "harness.o", "-o", "harness.elf"],
            cwd=root,
            check=True,
        )
        result = (root / "harness.elf").read_bytes()
        Path(destination).write_bytes(result)
        Path(destination).chmod(0o700)
    return {
        "delta_ms": manifest["delta_ms"],
        "adaptive_ll_delta_ms": ll_delta_ms,
        "vectors": len(cases),
        "candidate_sha256": sha256(candidate),
        "harness_sha256": sha256(result),
    }
