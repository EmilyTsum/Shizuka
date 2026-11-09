#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify an exact DSP trial and build/run its actual instruction-packet harness."""

import argparse
import json
import shutil
import subprocess
from pathlib import Path

from shizuka.ttp import verify_candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument(
        "--profile", help="explicit runtime profile; never infer support from a codec name"
    )
    parser.add_argument(
        "--candidate", type=Path, required=True, help="directory produced by ttp_trial.py"
    )
    parser.add_argument("--output", type=Path, required=True, help="Linux Hexagon harness ELF")
    parser.add_argument("--llvm-mc", default=shutil.which("llvm-mc"))
    parser.add_argument("--linker", default=shutil.which("ld.lld"))
    parser.add_argument(
        "--qemu", help="optional path to qemu-hexagon; otherwise run the output on Linux"
    )
    args = parser.parse_args()
    if not args.llvm_mc or not args.linker:
        parser.error("Set --llvm-mc and --linker to LLVM tools with Hexagon support")
    manifest = json.loads((args.candidate / "manifest.json").read_text())
    # The image filename is part of the backend's verified target descriptor.
    result = verify_candidate(
        args.input.read_bytes(),
        (args.candidate / Path(manifest["target_path"]).name).read_bytes(),
        manifest,
        args.output,
        args.llvm_mc,
        args.linker,
        args.profile,
    )
    if args.qemu:
        subprocess.run([args.qemu, str(args.output.resolve())], check=True, timeout=30)
        result["passed"] = True
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
