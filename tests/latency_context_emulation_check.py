#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual ARM64 HAL tag/strip hooks with synthetic memory, without vendor calls.

Requires an already-built native backend, pyelftools and Unicorn. The Report test
stops before resuming the original vendor body: it proves the wrapper's argument,
callee-saved register, PAC prologue/stack replay, and extension-removal contract.
No library is dlopened and no GPR/driver/device access occurs.
"""
import argparse
import io
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from elftools.elf.elffile import ELFFile
from shizuka.project import BUILD, inputs_dir
from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn import arm64_const as reg

CFG, STREAM, SESSION = 0x6000000, 0x6001000, 0x6002000
STACK, SP, RETURN = 0x7000000, 0x700F000, 0x7100000
CONTEXT_MAGIC = 0x31585A53


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def fixture(flags, length=27, corrupt=False):
    rate = 96000 if flags & 3 == 1 else 44100 if flags & 3 == 2 else 48000
    mode = 8192 if flags & 4 else 4096
    cfg = bytearray(112)
    for offset, value in ((0, 9), (0x58, 4), (0x68, 1), (0x18, rate),
                          (0x24, mode), (0x38, 255)):
        struct.pack_into("<I", cfg, offset, value)
    cfg[0x20] = 24
    cfg[0x28:0x2E] = bytes((20, 50, 60 if flags & 8 else 20, 100 if flags & 8 else 50, 20, 50))
    stream = bytearray((1, 0x97, 0, 0, 15, 2, 3, 3, 3, 0, 0xAA))
    normalized = (b"ACF1" + struct.pack("<IIII", rate, 0, 24, mode) +
                  cfg[0x28:0x34] + struct.pack("<II", 0, 255) + stream)
    fingerprint = fnv(normalized)
    tag = struct.pack("<III", 0x315A4853, flags, rate)
    stream += tag + struct.pack("<I", fnv(tag + struct.pack("<I", fingerprint)))
    if corrupt:
        stream[-1] ^= 1
    struct.pack_into("<QQQ", cfg, 0x40, STREAM, STREAM + length, STREAM + 27)
    return cfg, stream, fingerprint, rate


def image(raw):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    elf = ELFFile(io.BytesIO(raw))
    pages = set()
    for s in elf.iter_segments():
        if s["p_type"] == "PT_LOAD":
            pages.update(range(s["p_vaddr"] & ~4095,
                               (s["p_vaddr"] + s["p_memsz"] + 4095) & ~4095, 4096))
    for p in sorted(pages):
        u.mem_map(p, 4096)
    for s in elf.iter_segments():
        if s["p_type"] == "PT_LOAD" and s["p_filesz"]:
            u.mem_write(s["p_vaddr"], s.data())
    u.mem_map(CFG, 0x4000)
    u.mem_map(STACK, 0x10000)
    u.mem_map(RETURN, 4096)
    return u


def run(patched, symbols, flags, *, update=False, length=27, corrupt=False):
    u = image(patched)
    cfg, stream, fp, rate = fixture(flags, length, corrupt)
    u.mem_write(CFG, bytes(cfg))
    u.mem_write(STREAM, bytes(stream))
    session = bytearray(0x30)
    session[0] = session[0x28] = 2
    u.mem_write(SESSION, bytes(session))
    u.reg_write(reg.UC_ARM64_REG_X0, SESSION)
    u.reg_write(reg.UC_ARM64_REG_X1, CFG)
    u.reg_write(reg.UC_ARM64_REG_SP, SP)
    u.reg_write(reg.UC_ARM64_REG_LR, RETURN)
    saved = {getattr(reg, "UC_ARM64_REG_X" + str(i)): 0xAB0000 + i for i in range(19, 30)}
    for register, value in saved.items():
        u.reg_write(register, value)
    stops = []
    stop_at = 0x1BC84 if update else RETURN

    def stop(uc, address, size, data):
        if address == stop_at:
            stops.append(address)
            uc.emu_stop()

    u.hook_add(UC_HOOK_CODE, stop)
    u.emu_start(0x1BC7C if update else 0xE988, 0, count=100000)
    assert stops == [stop_at]
    for register, value in saved.items():
        assert u.reg_read(register) == value, (register, "callee-saved register changed")
    assert u.reg_read(reg.UC_ARM64_REG_SP) == SP - (0xC0 if update else 0)
    if update:
        assert u.reg_read(reg.UC_ARM64_REG_X0) == SESSION
        assert u.reg_read(reg.UC_ARM64_REG_X1) == CFG
        assert u.reg_read(reg.UC_ARM64_REG_LR) == RETURN
    else:
        assert u.reg_read(reg.UC_ARM64_REG_X0) == (not corrupt)
    begin, end, capacity = struct.unpack("<QQQ", u.mem_read(CFG + 0x40, 24))
    assert (begin, end, capacity) == (STREAM, STREAM + 11, STREAM + 27)
    assert u.mem_read(STREAM, 11) == stream[:11]
    c = struct.unpack("<16I", u.mem_read(symbols["s25_latency_context"], 64))
    assert c[:3] == (CONTEXT_MAGIC, 1, 64) and c[3] and not c[3] & 1
    assert c[4] == (length == 27 and not corrupt)
    if c[4]:
        mode = 12288 if flags & 8 else 8192 if flags & 4 else 4096
        assert c[5:9] == (rate, mode, flags, fp)
    assert c[9:] == (0,) * 7
    return {"entry": "ReportAudioConfigChanged" if update else "validator", "flags": flags,
            "input_bytes": length, "corrupt_tag": corrupt, "valid_context": bool(c[4])}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, default=BUILD)
    p.add_argument("--stock", type=Path, default=inputs_dir() / "libsehbluetooth_audio_session_aidl.so")
    a = p.parse_args()
    payload = ELFFile(io.BytesIO((a.build / "hal-payload.elf").read_bytes()))
    symbols = {s.name: s["st_value"] for s in payload.get_section_by_name(".symtab").iter_symbols()}
    stock = ELFFile(io.BytesIO(a.stock.read_bytes()))
    patched = (a.build / "patched-libsehbluetooth_audio_session_aidl.so").read_bytes()
    # Verify exactly the stock stolen PACIASP/SUB pair is replayed before its resume branch.
    for entry, name in ((0xE988, "validate"), (0x1BC7C, "report_config")):
        def read(e, addr, n):
            for s in e.iter_segments():
                if s["p_type"] == "PT_LOAD" and s["p_vaddr"] <= addr and addr + n <= s["p_vaddr"] + s["p_filesz"]:
                    return s.data()[addr-s["p_vaddr"]:addr-s["p_vaddr"]+n]
            raise AssertionError("unmapped ELF address")
        assert read(stock, entry, 8) == read(payload, symbols["tramp_" + name] + 4, 8)
    rows = []
    for update in (False, True):
        for flags in (1, 2, 6, 10):
            rows.append(run(patched, symbols, flags, update=update))
        rows.append(run(patched, symbols, 2, update=update, length=11))
        rows.append(run(patched, symbols, 2, update=update, corrupt=True))
    print(json.dumps({"passed": len(rows), "device_access": False, "results": rows}, indent=2))


if __name__ == "__main__":
    main()
