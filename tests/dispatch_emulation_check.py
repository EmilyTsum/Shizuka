#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual stock/current AArch64 dispatch prefixes; no copied CMP policy.
Usage: python tests/dispatch_emulation_check.py [PRISTINE_LIB BUILT_PATCHED_LIB]
Defaults use SHIZUKA_BASE_BIN (or inputs/) and build/patched-libbluetooth_jni.so.
Requires pyelftools and Unicorn. Does not modify either input.
"""

import argparse
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from elftools.elf.elffile import ELFFile
from shizuka.project import inputs_dir
from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn.arm64_const import UC_ARM64_REG_W8

ROUTES = {
    "hal": (
        0x8561C4,
        {
            0x856430: "sbc",
            0x85641C: "aac",
            0x8561E0: "aptx-converter",
            0x85645C: "ldac",
            0x856470: "adaptive",
            0x856344: "ssc",
            0x856648: "unknown",
        },
    ),
    "offload": (
        0x64961C,
        {
            0x649678: "sbc",
            0x649734: "aac",
            0x649658: "aptx-frame4",
            0x649724: "hd-frame8",
            0x64963C: "ldac-frame16",
            0x64972C: "adaptive-frame64",
            0x649670: "ssc-frame32",
            0x6496F0: "unknown",
        },
    ),
}


def read_virtual(raw, address, n):
    for seg in ELFFile(io.BytesIO(raw)).iter_segments():
        if (
            seg["p_type"] == "PT_LOAD"
            and seg["p_vaddr"] <= address
            and address + n <= seg["p_vaddr"] + seg["p_filesz"]
        ):
            off = seg["p_offset"] + address - seg["p_vaddr"]
            return raw[off : off + n]
    raise AssertionError("unmapped file address " + hex(address))


def branch_target(word, pc):
    assert word & 0xFC000000 == 0x14000000, ("not B", hex(pc), hex(word))
    v = word & 0x3FFFFFF
    if v & (1 << 25):
        v -= 1 << 26
    return pc + 4 * v


def replay_exact(old, new, start):
    dest = branch_target(struct.unpack("<I", read_virtual(new, start, 4))[0], start)
    return read_virtual(old, start, 4) == read_virtual(new, dest + 12, 4)


def run(raw, kind, index):
    start, leaves = ROUTES[kind]
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    word = struct.unpack("<I", read_virtual(raw, start, 4))[0]
    pages = {start & ~4095}
    if word & 0xFC000000 == 0x14000000:
        pages.add(branch_target(word, start) & ~4095)
    for page in pages:
        u.mem_map(page, 4096)
        for seg in ELFFile(io.BytesIO(raw)).iter_segments():
            if seg["p_type"] != "PT_LOAD":
                continue
            lo = max(page, seg["p_vaddr"])
            hi = min(page + 4096, seg["p_vaddr"] + seg["p_filesz"])
            if lo < hi:
                off = seg["p_offset"] + lo - seg["p_vaddr"]
                u.mem_write(lo, raw[off : off + hi - lo])
    result = []

    def stop(uc, address, size, data):
        if address in leaves:
            result.append(leaves[address])
            uc.emu_stop()

    u.hook_add(UC_HOOK_CODE, stop)
    u.reg_write(UC_ARM64_REG_W8, index)
    u.emu_start(start, 0, count=80)
    assert len(result) == 1, (kind, index, result)
    return result[0]


def check_dispatch(old_path, new_path):
    old = Path(old_path).read_bytes()
    new = Path(new_path).read_bytes()
    report = {
        "stock_sha256": hashlib.sha256(old).hexdigest(),
        "new_sha256": hashlib.sha256(new).hexdigest(),
        "results": [],
        "failures": [],
        "replay_exact": {},
    }
    # Existing Android enum/source/sink indices plus signed-boundary unsupported values.
    for kind, (start, _) in ROUTES.items():
        report["replay_exact"][kind] = replay_exact(old, new, start)
        if not report["replay_exact"][kind]:
            report["failures"].append(kind + " overwritten CMP not replayed exactly")
        for index in [*range(17), 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000]:
            pristine = run(old, kind, index)
            actual = run(new, kind, index)
            expected = pristine
            if index == 5:
                expected = "aptx-converter" if kind == "hal" else "hd-frame8"
            if index == 9:
                expected = "aptx-converter" if kind == "hal" else "aptx-frame4"
            row = {
                "route": kind,
                "codec_index": index,
                "stock": pristine,
                "expected": expected,
                "actual": actual,
            }
            report["results"].append(row)
            if actual != expected:
                report["failures"].append(row)
    return report


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "stock",
        nargs="?",
        type=Path,
        default=inputs_dir() / "libbluetooth_jni.so",
    )
    p.add_argument(
        "patched", nargs="?", type=Path, default=root / "build/patched-libbluetooth_jni.so"
    )
    a = p.parse_args()
    r = check_dispatch(a.stock, a.patched)
    print(json.dumps(r, indent=2))
    raise SystemExit(bool(r["failures"]))
