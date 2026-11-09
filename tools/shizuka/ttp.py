# SPDX-License-Identifier: GPL-2.0-only
"""Offline, exact-image HQ TTP experiment. Nothing here changes a connected phone."""

import hashlib
import io
import json
import struct
import subprocess
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile

STOCK_SHA256 = "05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3"
TARGET_PATH = "/vendor/dsp/adsp/aptx_adaptive_enc_module.so.1"
HOOK = 0x14FB4
CAVE = 0x23A30
LIMIT = 0x24000
ORIGINAL_PACKET = bytes.fromhex("ff7fff0f8243929114e04224")
LOG_OFFSET = 0x21CDD
LOG = b"CAPI V2 APTX ADAPTIVE ENC Calculated ttpadj: ttpadj microseconds %lld\0"
MAX_WIRE_US = 65535 * 8


def validate_control_plan(plan):
    """Validate the versioned public plan boundary before invoking build tools."""
    if not isinstance(plan, dict):
        raise ValueError("Invalid codec-control plan")
    context = plan.get("context", {})
    increment = plan.get("requested_ms")
    if (
        type(plan.get("schema")) is not int
        or plan.get("schema") != 1
        or plan.get("provider") != "samsung-dzif-calculated-hq-ttp"
        or plan.get("applied") is not False
        or plan.get("activation") != "offline_dsp_build_then_fresh_graph"
        or plan.get("coupled_contexts") != ["adaptive/hq", "lossless/hq"]
        or type(increment) is not int
        or increment not in (0, 50, 100)
        or plan.get("state") != ("stock" if increment == 0 else "planned_not_applied")
        or not isinstance(context, dict)
        or context.get("profile") != "samsung-sm-s931z-dzif"
        or context.get("codec") not in ("adaptive", "lossless")
        or context.get("mode") != "hq"
        or type(context.get("sample_rate_hz")) is not int
        or type(context.get("selection_bits")) is not int
        or context.get("sample_rate_hz") not in (44100, 48000, 96000)
        or context.get("selection_bits") != (16 if context.get("codec") == "lossless" else 24)
        or (context.get("codec") == "lossless" and context.get("sample_rate_hz") != 44100)
    ):
        raise ValueError("Unsupported or inconsistent codec-control plan")
    return increment


# Only caller-saved r2-r7 and predicates are scratch. r16/r18/r19/r31 and the
# existing frame remain intact. The original debug dispatch packet is replayed,
# including its .new dependency, before either original log path continues.
ASM = """
.section .hook,"ax",@progbits
{ jump s25_ttp_bias }
{ nop }
{ nop }
.section .bias,"ax",@progbits
.global s25_ttp_bias
s25_ttp_bias:
{ r2 = memw(r19+#0) }
{ p0 = cmp.eq(r2,#3); p1 = cmp.eq(r2,#7) }
{ p0 = or(p0,p1) }
{ if (!p0) jump .Lresume }
{ r7:6 = combine(#0,##524280) }
{ p0 = cmp.gtu(r21:20,r7:6) }
{ if (p0) jump .Lresume }
{ r3:2 = combine(#0,##@DELTA_US@) }
{ r5:4 = add(r21:20,r3:2) }
{ r2 = memw(r16+##0x6494) }
{ r3 = #1000 }
{ r3:2 = mpyu(r2,r3) }
{ r3:2 = add(r5:4,r3:2) }
{ p0 = cmp.gtu(r3:2,r7:6) }
{ if (p0) jump .Lresume }
{ r21:20 = r5:4 }
.Lresume:
{ r2 = memw(r18+##-0x24)
  if (!cmp.eq(r2.new,#0)) jump:t s25_log_long }
{ jump s25_log_short }
"""
LINKER = """ENTRY(s25_ttp_bias)
s25_log_long = 0x14fdc;
s25_log_short = 0x14fc0;
SECTIONS {
 .hook 0x14fb4 : { *(.hook) }
 .bias 0x23a30 : { *(.bias) }
 /DISCARD/ : { *(.text) *(.comment) *(.note*) }
}
"""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def inspect_stock(data):
    if sha256(data) != STOCK_SHA256:
        raise ValueError("Unsupported DSP image: stock SHA-256 must match exactly")
    elf = ELFFile(io.BytesIO(data))
    if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_QDSP6":
        raise ValueError("Expected little-endian ELF32 Hexagon")
    segment = elf.get_segment(0)
    if (
        segment["p_type"] != "PT_LOAD"
        or segment["p_flags"] != 5
        or segment["p_offset"] != 0
        or segment["p_vaddr"] != 0
        or segment["p_filesz"] != 0x23A2C
        or segment["p_memsz"] != 0x23A2C
    ):
        raise ValueError("Unexpected executable segment layout")
    if any(data[0x23A2C:LIMIT]) or len(data) < LIMIT:
        raise ValueError("Executable padding is absent or occupied")
    if data[HOOK : HOOK + len(ORIGINAL_PACKET)] != ORIGINAL_PACKET:
        raise ValueError("calcTTPAdj debug packet does not match")
    if data[LOG_OFFSET : LOG_OFFSET + len(LOG)] != LOG:
        raise ValueError("calcTTPAdj diagnostic string does not match")
    symbols = elf.get_section_by_name(".symtab")
    for name, address in (("calcTTPAdj", 0x14DC0), ("setEncoderCfgVals", 0x15030)):
        entries = symbols.get_symbol_by_name(name) if symbols else None
        if not entries or len(entries) != 1 or entries[0]["st_value"] != address:
            raise ValueError(f"Unexpected symbol: {name}")
    return {
        "stock_sha256": STOCK_SHA256,
        "target_path": TARGET_PATH,
        "compiled_hq_target_word": 70,
        "compiled_hq_target_unit_us": 4000,
        "compiled_hq_alternate_word": 70,
        "live_targets": None,
        "note": "Compiled defaults are not live per-mode measurements or receiver latency.",
    }


def compile_patch(delta_ms, llvm_mc, linker):
    if type(delta_ms) is not int or delta_ms not in (50, 100):
        raise ValueError("Only +50 ms and +100 ms trials are supported")
    with tempfile.TemporaryDirectory(prefix="shizuka-ttp-") as directory:
        root = Path(directory)
        source = root / "bias.s"
        script = root / "bias.ld"
        source.write_text(ASM.replace("@DELTA_US@", str(delta_ms * 1000)))
        script.write_text(LINKER)
        subprocess.run(
            [
                str(llvm_mc),
                "-triple=hexagon",
                "-mcpu=hexagonv73",
                "-filetype=obj",
                str(source),
                "-o",
                str(root / "bias.o"),
            ],
            check=True,
        )
        subprocess.run(
            [
                str(linker),
                "--no-relax",
                "-T",
                str(script),
                str(root / "bias.o"),
                "-o",
                str(root / "bias.elf"),
            ],
            check=True,
        )
        elf = ELFFile(io.BytesIO((root / "bias.elf").read_bytes()))
        pieces = {}
        for name, address in ((".hook", HOOK), (".bias", CAVE)):
            section = elf.get_section_by_name(name)
            if section is None or section["sh_addr"] != address:
                raise ValueError("Unexpected patch placement")
            pieces[name] = section.data()
        if len(pieces[".hook"]) != len(ORIGINAL_PACKET):
            raise ValueError("Hook must replace the whole original instruction packet")
        if not pieces[".bias"] or CAVE + len(pieces[".bias"]) >= LIMIT:
            raise ValueError("Patch overlaps the following load segment")
        return pieces


def patch_verified(data, delta_ms, pieces):
    """Recheck every gate even when called outside the command-line builder."""
    baseline = inspect_stock(data)
    if type(delta_ms) is not int or delta_ms not in (50, 100):
        raise ValueError("Only +50 ms and +100 ms trials are supported")
    if set(pieces) != {".hook", ".bias"} or len(pieces[".hook"]) != 12:
        raise ValueError("Invalid patch sections")
    end = CAVE + len(pieces[".bias"])
    if end <= CAVE or end >= LIMIT:
        raise ValueError("Invalid patch size")
    result = bytearray(data)
    allowed = [[HOOK, HOOK + 12], [CAVE, end]]
    result[HOOK : HOOK + 12] = pieces[".hook"]
    result[CAVE:end] = pieces[".bias"]
    elf = ELFFile(io.BytesIO(data))
    header = elf["e_phoff"] + 16
    struct.pack_into("<II", result, header, end, end)
    allowed.append([header, header + 8])
    # Both existing log paths now observe the result *after* the bounded bias.
    # "requested" distinguishes configuration from application when the ceiling
    # guard declines an increment, or when this is an LL/other-state invocation.
    label = f"Shizuka TTP HQ requested +{delta_ms}ms; calculated us %lld\n".encode() + b"\0"
    if len(label) > len(LOG):
        raise ValueError("Diagnostic label does not fit")
    result[LOG_OFFSET : LOG_OFFSET + len(LOG)] = label.ljust(len(LOG), b"\0")
    allowed.append([LOG_OFFSET, LOG_OFFSET + len(LOG)])
    if len(result) != len(data) or any(
        not any(start <= index < stop for start, stop in allowed)
        for index, (old, new) in enumerate(zip(data, result))
        if old != new
    ):
        raise ValueError("Patch changed bytes outside the declared ranges")
    manifest = {
        "schema": 1,
        **baseline,
        "delta_ms": delta_ms,
        "output_sha256": sha256(result),
        "modified_ranges": allowed,
        "rx_end": end,
        "states": [3, 7],
        "wire_ceiling_us": MAX_WIRE_US,
        "semantics": "Add to HQ TTP adjustment after stock clamps/floors, before logging/packing; include final state term in ceiling check. Skip increment if it cannot fit.",
        "target_words_changed": False,
        "installed": False,
        "audio_verified": False,
        "receiver_delay_verified": False,
    }
    return bytes(result), manifest


def write_candidate(stock, destination, delta_ms, llvm_mc, linker, control_plan=None):
    data = Path(stock).read_bytes()
    baseline = inspect_stock(data)
    if control_plan is not None and validate_control_plan(control_plan) != delta_ms:
        raise ValueError("TTP increment differs from the control plan")
    if type(delta_ms) is not int or delta_ms not in (0, 50, 100):
        raise ValueError("Only stock (0), +50 ms and +100 ms trials are supported")
    if delta_ms:
        pieces = compile_patch(delta_ms, llvm_mc, linker)
        result, manifest = patch_verified(data, delta_ms, pieces)
    else:
        result = data
        manifest = {
            "schema": 1,
            **baseline,
            "delta_ms": 0,
            "output_sha256": sha256(data),
            "modified_ranges": [],
            "rx_end": 0x23A2C,
            "states": [],
            "wire_ceiling_us": MAX_WIRE_US,
            "semantics": "Unmodified stock control image",
            "target_words_changed": False,
            "installed": False,
            "audio_verified": False,
            "receiver_delay_verified": False,
        }
    if control_plan is not None:
        manifest["control_plan"] = control_plan
    destination = Path(destination)
    # Refuse existing paths: never replace a currently mounted experiment.
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "aptx_adaptive_enc_module.so.1").write_bytes(result)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if delta_ms:
        (destination / "bias.s").write_text(ASM.replace("@DELTA_US@", str(delta_ms * 1000)))
    return manifest
