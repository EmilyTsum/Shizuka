# SPDX-License-Identifier: GPL-2.0-only
"""Bounded LL private9 veneers and original Adaptive private5 dispatch."""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from elf_query import Binary

root = Path(__file__).resolve().parents[1]
old = Binary(Path(os.environ.get("S25_BASE_BIN", str(root / "inputs"))) / "libbluetooth_jni.so")
new = Binary(root / "build/patched-libbluetooth_jni.so")


def mapped_read(address, size):
    for segment in new.elf.iter_segments():
        if (
            segment["p_type"] == "PT_LOAD"
            and segment["p_vaddr"] <= address
            and address + size <= segment["p_vaddr"] + segment["p_filesz"]
        ):
            return segment.data()[
                address - segment["p_vaddr"] : address - segment["p_vaddr"] + size
            ]
    raise AssertionError(hex(address))


for start, size, adaptive, target, gate, match, resume in [
    (0x8561C4, 0x298, 0x856458, 0x8561E0, 0x8561C4, 0x8561E0, 0x8561C8),
    (0x64961C, 0x120, 0x649638, 0x649724, 0x64961C, 0x649658, 0x649620),
]:
    a = old.read(start, size)
    b = new.read(start, size)
    for offset in range(0, size, 4):
        if start + offset not in (gate, adaptive):
            assert a[offset : offset + 4] == b[offset : offset + 4], hex(start + offset)
    branch = list(new.md.disasm(new.read(adaptive, 4), adaptive))[0]
    assert branch.mnemonic == "b.ne" and branch.operands[0].imm == target
    jump = list(new.md.disasm(new.read(gate, 4), gate))[0]
    assert jump.mnemonic == "b"
    address = jump.operands[0].imm
    ins = list(new.md.disasm(mapped_read(address, 20), address))
    assert ins[0].mnemonic == "cmp" and ins[0].op_str == "w8, #9"
    assert ins[1].mnemonic == "b.ne" and ins[1].operands[0].imm == address + 12
    assert ins[2].mnemonic == "b" and ins[2].operands[0].imm == match
    assert ins[3].mnemonic == "cmp" and ins[3].op_str == (
        "w8, #5" if start == 0x8561C4 else "w8, #3"
    )
    assert ins[4].mnemonic == "b" and ins[4].operands[0].imm == resume
    print(
        hex(gate),
        "private9 only ->",
        hex(match),
        "; other indices resume original dispatch; private5 branch preserved",
    )
old_cmp = list(old.md.disasm(old.read(0x95E6D4, 4), 0x95E6D4))[0]
new_cmp = list(new.md.disasm(new.read(0x95E6D4, 4), 0x95E6D4))[0]
assert old_cmp.op_str == "w23, #9" and new_cmp.op_str == "w23, #0xa"
print(
    "Source initialization extends exclusive bound9 ->10; original0..8 retained, sink indices13+ unchanged"
)
