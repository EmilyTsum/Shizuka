# SPDX-License-Identifier: GPL-2.0-only
import json
import re
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, Cs, arm64
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG
from elftools.elf.elffile import ELFFile


class Binary:
    def __init__(self, path):
        self.path = Path(path)
        self.f = self.path.open("rb")
        self.elf = ELFFile(self.f)
        self.sections = list(self.elf.iter_sections())
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
        self.md.detail = True

    def read(self, addr, size):
        for s in self.sections:
            if s["sh_type"] != "SHT_NOBITS" and s["sh_addr"] <= addr < s["sh_addr"] + s["sh_size"]:
                return s.data()[addr - s["sh_addr"] : addr - s["sh_addr"] + size]
        return b""

    def string(self, addr):
        d = self.read(addr, 200).split(b"\0")[0]
        return d.decode("utf8") if d and all(x in [9, 10, 13] or 32 <= x < 127 for x in d) else None

    def dis(self, addr, size):
        regs = {}
        for i in self.md.disasm(self.read(addr, size), addr):
            note = ""
            ops = i.operands
            target = None
            if i.mnemonic in ("adr", "adrp") and len(ops) > 1 and ops[1].type == ARM64_OP_IMM:
                regs[ops[0].reg] = ops[1].imm
                target = ops[1].imm
            elif (
                i.mnemonic == "add"
                and len(ops) == 3
                and ops[1].type == ARM64_OP_REG
                and ops[2].type == ARM64_OP_IMM
            ):
                if ops[1].reg in regs:
                    target = regs[ops[1].reg] + ops[2].imm
                    regs[ops[0].reg] = target
            else:
                for rr in i.regs_access()[1]:
                    regs.pop(rr, None)
            if target is not None:
                st = self.string(target)
                if st:
                    note = " ; string " + hex(target) + " " + repr(st)
            print(f"{i.address:08x}: {i.mnemonic:8} {i.op_str}{note}")
            if i.mnemonic in ("bl", "blr"):
                for n in range(19):
                    regs.pop(
                        i.reg_name(0) if False else getattr(arm64, "ARM64_REG_X" + str(n), -1),
                        None,
                    )
                    regs.pop(getattr(arm64, "ARM64_REG_W" + str(n), -1), None)
            elif i.mnemonic == "ret":
                regs.clear()

    def xrefs(self, pat):
        wanted = {}
        for s in self.sections:
            if s["sh_type"] == "SHT_NOBITS" or s["sh_flags"] & 4:
                continue
            data = s.data()
            for m in re.finditer(rb"[^\x00]{3,}\x00", data):
                st = m.group()[:-1].decode("utf8", errors="replace")
                if re.search(pat, st, re.I):
                    wanted[s["sh_addr"] + m.start()] = st
        print(
            "STRINGS",
            json.dumps({hex(k): v for k, v in wanted.items()}, ensure_ascii=False),
        )
        for s in self.sections:
            if not s["sh_flags"] & 4:
                continue
            regs = {}
            for i in self.md.disasm(s.data(), s["sh_addr"]):
                ops = i.operands
                target = None
                if i.mnemonic in ("adr", "adrp") and len(ops) > 1 and ops[1].type == ARM64_OP_IMM:
                    regs[ops[0].reg] = ops[1].imm
                    target = ops[1].imm
                elif (
                    i.mnemonic == "add"
                    and len(ops) == 3
                    and ops[1].type == ARM64_OP_REG
                    and ops[2].type == ARM64_OP_IMM
                ):
                    if ops[1].reg in regs:
                        target = regs[ops[1].reg] + ops[2].imm
                        regs[ops[0].reg] = target
                else:
                    for rr in i.regs_access()[1]:
                        regs.pop(rr, None)
                if target in wanted:
                    print(hex(i.address), i.mnemonic, i.op_str, repr(wanted[target]))


if __name__ == "__main__":
    b = Binary(sys.argv[1])
    if sys.argv[2] == "dis":
        b.dis(int(sys.argv[3], 0), int(sys.argv[4], 0))
    elif sys.argv[2] == "xref":
        b.xrefs(sys.argv[3])
