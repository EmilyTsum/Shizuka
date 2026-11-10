#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the root-side lease publisher; never deploy, run on Android or open GPR."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ndk-bin", type=Path, default=os.environ.get("SHIZUKA_NDK_BIN"))
    parser.add_argument("--host", action="store_true")
    args = parser.parse_args()
    base = Path(__file__).resolve().parent
    checkout = base.parents[2]
    output = args.output.expanduser().resolve()
    if output.is_relative_to(checkout):
        parser.error("put generated binaries outside the source checkout")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not args.host and args.ndk_bin is None:
        parser.error("--ndk-bin or SHIZUKA_NDK_BIN is required")
    native = base.parent / "native"
    compiler = "clang++" if args.host else str(args.ndk_bin / "aarch64-linux-android34-clang++")
    flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "-I" + str(base / "include"), "-I" + str(native / "include")]
    if args.host:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    else:
        flags += ["-static-libstdc++", "-mbranch-protection=standard", "-Wl,-z,max-page-size=16384"]
    shared = [base / "src/record.cpp", base / "src/status.cpp", base / "src/atomic_publisher.cpp",
              native / "src/external_config.cpp"]
    subprocess.run([compiler, *flags, *map(str, shared), str(base / "src/main.cpp"), "-o", str(output)], check=True)
    metadata = {"schema": 1, "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "sources_sha256": {str(p.relative_to(checkout)): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in [*shared, base / "src/main.cpp", base / "include/shizuka/provider.hpp"]},
                "android": not args.host, "gpr_linked": False, "device_executed": False}
    if args.host:
        test = output.with_name(output.name + "-test")
        subprocess.run([compiler, *flags, *map(str, shared), str(base / "tests/provider_test.cpp"), "-o", str(test)], check=True)
        result = subprocess.check_output([str(test)], text=True)
        metadata["host_asan_ubsan"] = result.strip()
        print(result.strip())
    output.with_suffix(".build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
