#!/usr/bin/env python3
"""Build the embedded exact-device runtime; outputs stay outside the checkout."""

import argparse
import hashlib
import io
import json
import os
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ndk-bin", type=Path, default=os.environ.get("SHIZUKA_NDK_BIN"))
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cookie", required=True)
    p.add_argument(
        "--attach",
        action="store_true",
        help="register through the existing HAL dispatcher; no graph changes",
    )
    p.add_argument("--relay", action="store_true", help="join the exact fresh owned HQ44.1 graph")
    p.add_argument(
        "--diagnostic-zero-ttp",
        action="store_true",
        help="root diagnostic only: retain relay but never add packet TTP; cannot be packaged",
    )
    p.add_argument(
        "--experimental-raw-island",
        action="store_true",
        help="source experiment only: separate owned RAW WR container; cannot be packaged",
    )
    p.add_argument(
        "--hal-payload", type=Path, help="matching final HAL payload ELF with typed context symbol"
    )
    a = p.parse_args()
    base = Path(__file__).resolve().parent
    if a.ndk_bin is None:
        p.error("--ndk-bin is required")
    if a.diagnostic_zero_ttp and not a.relay:
        p.error("--diagnostic-zero-ttp requires --relay")
    if a.experimental_raw_island and not a.relay:
        p.error("--experimental-raw-island requires --relay")
    if len(a.cookie) != 32 or any(c not in "0123456789abcdef" for c in a.cookie):
        p.error("cookie must be 16 lowercase hex bytes")
    context_rva = 0
    if a.relay:
        if not a.hal_payload:
            p.error("--relay requires --hal-payload")
        hal = ELFFile(io.BytesIO(a.hal_payload.read_bytes()))
        values = [
            s["st_value"]
            for s in hal.get_section_by_name(".symtab").iter_symbols()
            if s.name == "s25_latency_context"
        ]
        if len(values) != 1 or values[0] % 4:
            p.error("missing/alignment-invalid HAL context")
        context_rva = values[0]
    out = a.output.resolve()
    if any((v / ".git").exists() for v in [out.parent, *out.parents]):
        p.error("build output must be outside a Git checkout")
    out.parent.mkdir(parents=True, exist_ok=True)
    (out.parent / "activation_cookie.h").write_text(
        "static const unsigned char shizuka_activation_cookie[16]={"
        + ",".join("0x" + a.cookie[i : i + 2] for i in range(0, 32, 2))
        + "};\n"
    )
    flags = [
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-fvisibility=hidden",
        "-fPIC",
        "-mbranch-protection=standard",
        "-mno-outline-atomics",
        "-DSHIZUKA_ATTACH=" + str(int(a.attach or a.relay)),
        "-DSHIZUKA_RELAY=" + str(int(a.relay)),
        "-DSHIZUKA_DIAG_ZERO_TTP=" + str(int(a.diagnostic_zero_ttp)),
        "-DSHIZUKA_RELAY_RAW_ISLAND=" + str(int(a.experimental_raw_island)),
        "-DSHIZUKA_HAL_CONTEXT_RVA=" + str(context_rva),
    ]
    roots = [
        base / "include",
        base.parent / "transport/native/include",
        base.parent / "gsl_hook",
        out.parent,
    ]
    sources = [
        base / "embedded_runtime.c",
        base / "gateways.S",
        base / "src/vendor_adapter.c",
        base / "src/encoded_transform.c",
        base / "src/control_ack.c",
        base / "src/control_scope.c",
        base / "src/receive_trace.c",
        base / "src/data_receipt_trace.c",
        base / "src/init_profile.c",
        base.parent / "transport/native/src/hal_transport.c",
        base / "graph_bridge.c",
        base / "default_cal_context.c",
        *[
            base.parent / "gsl_hook" / n
            for n in ("relay_graph.c", "relay_transaction.c", "relay_calibration.c")
        ],
    ]
    commands = []
    objects = []
    for s in sources:
        obj = out.parent / (s.stem + ".o")
        cmd = [
            str(a.ndk_bin / "aarch64-linux-android34-clang"),
            "-std=c11",
            *flags,
            *["-I" + str(v) for v in roots],
            "-c",
            str(s),
            "-o",
            str(obj),
        ]
        subprocess.run(cmd, check=True)
        commands.append(cmd)
        objects.append(obj)
        elf = ELFFile(io.BytesIO(obj.read_bytes()))
        allowed = {273, 274, 275, 276, 277, 278, 279, 280, 282, 283, 284, 285, 286, 299}
        for section in elf.iter_sections():
            if section["sh_type"] not in ("SHT_RELA", "SHT_REL"):
                continue
            if not elf.get_section(section["sh_info"])["sh_flags"] & 2:
                continue
            for rel in section.iter_relocations():
                assert rel["r_info_type"] in allowed, (
                    "non-PIC object relocation",
                    s,
                    rel["r_info_type"],
                )
    cmd = [
        str(a.ndk_bin / "aarch64-linux-android34-clang"),
        "-nostdlib",
        "-static",
        *map(str, objects),
        "-Wl,-T," + str(base / "runtime.ld"),
        "-Wl,--no-undefined",
        "-Wl,--build-id=none",
        "-Wl,-z,max-page-size=16384",
        "-o",
        str(out),
    ]
    subprocess.run(cmd, check=True)
    commands.append(cmd)
    raw = out.read_bytes()
    e = ELFFile(io.BytesIO(raw))
    sym = e.get_section_by_name(".symtab")
    undefined = [s.name for s in sym.iter_symbols() if s.name and s["st_shndx"] == "SHN_UNDEF"]
    assert not undefined, undefined
    diagnostic_modes = [
        s for s in sym.iter_symbols() if s.name == "shizuka_runtime_diagnostic_zero_ttp_mode"
    ]
    assert (
        len(diagnostic_modes) == 1
        and diagnostic_modes[0]["st_shndx"] == "SHN_ABS"
        and diagnostic_modes[0]["st_value"] == int(a.diagnostic_zero_ttp)
    ), "diagnostic ELF mode does not match requested build"
    raw_modes = [s for s in sym.iter_symbols() if s.name == "shizuka_runtime_raw_island_mode"]
    assert (
        len(raw_modes) == 1
        and raw_modes[0]["st_shndx"] == "SHN_ABS"
        and raw_modes[0]["st_value"] == int(a.experimental_raw_island)
    ), "RAW-island ELF mode does not match requested build"
    assert all(
        s.num_relocations() == 0
        for s in e.iter_sections()
        if s["sh_type"] in ("SHT_REL", "SHT_RELA")
    )
    segs = [s for s in e.iter_segments() if s["p_type"] == "PT_LOAD" and s["p_memsz"]]
    assert len(segs) == 2 and segs[0]["p_flags"] == 5 and segs[1]["p_flags"] == 6
    manifest = {
        "sha256": hashlib.sha256(raw).hexdigest(),
        "cookie": a.cookie,
        "commands": commands,
        "symbols": {
            s.name: hex(s["st_value"])
            for s in sym.iter_symbols()
            if s.name.startswith("shizuka_runtime_")
        },
        "segments": [dict(s.header) for s in segs],
        "observer_only": not a.relay,
        "attach_enabled": a.attach or a.relay,
        "relay_enabled": a.relay,
        "hal_context_rva": context_rva,
        "diagnostic_only": a.diagnostic_zero_ttp or a.experimental_raw_island,
        "diagnostic_zero_ttp": a.diagnostic_zero_ttp,
        "experimental_raw_island": a.experimental_raw_island,
        "gpr_initialized_by_runtime": False,
        "device_opened_by_runtime": False,
    }
    out.with_suffix(".build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({k: v for k, v in manifest.items() if k != "commands"}, indent=2))


if __name__ == "__main__":
    main()
