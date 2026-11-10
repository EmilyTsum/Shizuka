#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build optional source-bound latency artifacts; never activate or deploy them."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from elftools.common.exceptions import ELFError
from shizuka.artifacts import write_metadata
from shizuka.latency_artifacts import (
    EXTRA_OUTPUTS,
    LATENCY_BUILD,
    REVIEWED_DEPENDENCIES,
    latency_identity,
    verify_latency_runtime,
)
from shizuka.profiles import load_profile
from shizuka.project import BUILD, ROOT, digest, ndk_bin


def reviewed_input(path, target):
    path = Path(path).expanduser()
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 16 * 1024 * 1024:
        raise ValueError(f"Missing or unsafe reviewed runtime input: {path}")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != REVIEWED_DEPENDENCIES[target]:
        raise ValueError(f"Runtime dependency does not match the reviewed firmware: {target}")
    return data


def run(command, *, env=None):
    subprocess.run([str(arg) for arg in command], check=True, env=env)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gsl",
        type=Path,
        default=os.environ.get("SHIZUKA_GSL_INPUT"),
        help="immutable reviewed libar-gsl.so (or SHIZUKA_GSL_INPUT)",
    )
    parser.add_argument(
        "--gpr",
        type=Path,
        default=os.environ.get("SHIZUKA_GPR_INPUT"),
        help="reviewed libar-gpr.so; defaults to the GSL input's sibling",
    )
    parser.add_argument(
        "--osal",
        type=Path,
        default=os.environ.get("SHIZUKA_OSAL_INPUT"),
        help="reviewed liblx-osal.so; defaults to the GSL input's sibling",
    )
    parser.add_argument("--ndk-bin", type=Path)
    parser.add_argument(
        "--diagnostic-zero-ttp",
        action="store_true",
        help="root diagnostic only: relay without added TTP; outputs cannot be packaged",
    )
    parser.add_argument(
        "--experimental-raw-island",
        action="store_true",
        help="source experiment only: separate owned RAW WR container; outputs cannot be packaged",
    )
    args = parser.parse_args(argv)
    BUILD.mkdir(parents=True, exist_ok=True)
    # Invalidate prior completion before any operation that can fail.
    (BUILD / LATENCY_BUILD).unlink(missing_ok=True)
    if args.gsl is None:
        parser.error("--gsl or SHIZUKA_GSL_INPUT is required")
    compiler = args.ndk_bin.expanduser() if args.ndk_bin else ndk_bin()
    for name in ("aarch64-linux-android34-clang", "aarch64-linux-android34-clang++"):
        if not (compiler / name).is_file():
            raise ValueError(f"NDK compiler is missing: {compiler / name}")
    profile = load_profile()
    modes = dict(
        diagnostic_zero_ttp=args.diagnostic_zero_ttp,
        experimental_raw_island=args.experimental_raw_island,
    )
    diagnostic_only = any(modes.values())
    before = latency_identity(profile, BUILD, ROOT, **modes)
    inputs = {
        "libar-gsl.so": reviewed_input(args.gsl, "/vendor/lib64/libar-gsl.so"),
        "libar-gpr.so": reviewed_input(
            args.gpr or args.gsl.parent / "libar-gpr.so", "/vendor/lib64/libar-gpr.so"
        ),
        "liblx-osal.so": reviewed_input(
            args.osal or args.gsl.parent / "liblx-osal.so", "/vendor/lib64/liblx-osal.so"
        ),
    }
    base = ROOT / "experiments/latency"
    with tempfile.TemporaryDirectory(prefix="shizuka-latency-build-") as name:
        work = Path(name)
        stock = work / "stock"
        stock.mkdir()
        for filename, data in inputs.items():
            (stock / filename).write_bytes(data)
            (stock / filename).chmod(0o400)
        # Every component builds outside the checkout. Copy the final matching
        # HAL ELF too, so concurrent core rebuilds cannot change its input.
        hal = work / "hal-payload.elf"
        shutil.copyfile(BUILD / "hal-payload.elf", hal)
        runtime = work / "latency-runtime-payload.elf"
        runtime_command = [
            sys.executable,
            base / "runtime/build_runtime.py",
            "--ndk-bin",
            compiler,
            "--relay",
            "--hal-payload",
            hal,
            "--cookie",
            before["cookie"],
            "--output",
            runtime,
        ]
        if args.diagnostic_zero_ttp:
            runtime_command.append("--diagnostic-zero-ttp")
        if args.experimental_raw_island:
            runtime_command.append("--experimental-raw-island")
        run(runtime_command)
        candidate = work / "patched-libar-gsl.so"
        run(
            [
                sys.executable,
                base / "runtime/patch_runtime.py",
                "--stock",
                stock / "libar-gsl.so",
                "--payload",
                runtime,
                "--output",
                candidate,
            ]
        )
        env = dict(
            os.environ,
            SHIZUKA_GSL_STOCK=str(stock / "libar-gsl.so"),
            SHIZUKA_RUNTIME_PAYLOAD=str(runtime),
        )
        run([sys.executable, base / "runtime/tests/test_runtime.py"], env=env)
        provider = work / "latency-provider"
        run(
            [
                sys.executable,
                base / "provider/build_provider.py",
                "--host",
                "--output",
                work / "provider-host",
            ]
        )
        run(
            [
                sys.executable,
                base / "provider/build_provider.py",
                "--ndk-bin",
                compiler,
                "--output",
                provider,
            ]
        )
        if before != latency_identity(profile, BUILD, ROOT, **modes):
            raise ValueError(
                "Latency or core sources/artifacts changed during the build; rebuild them"
            )
        files = {
            "latency-provider": provider,
            "patched-libar-gsl.so": candidate,
            "latency-runtime-payload.elf": runtime,
            "latency-runtime-payload.build.json": runtime.with_suffix(".build.json"),
            "latency-runtime-patch.json": candidate.with_suffix(".manifest.json"),
            "latency-provider.build.json": provider.with_suffix(".build.json"),
        }
        metadata = dict(
            before, outputs={filename: digest(path) for filename, path in files.items()}
        )
        # Publish each completed artifact atomically, then publish completion
        # metadata last. A partial build is never packageable.
        for filename in EXTRA_OUTPUTS:
            target = BUILD / filename
            if target.is_symlink():
                raise ValueError(f"Latency build output must not be a symlink: {target}")
            temporary = BUILD / ("." + filename + ".new")
            if temporary.exists() or temporary.is_symlink():
                raise ValueError(f"Unresolved latency output staging file: {temporary}")
            try:
                shutil.copyfile(files[filename], temporary)
                temporary.chmod(0o755 if filename == "latency-provider" else 0o644)
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
        write_metadata(BUILD / LATENCY_BUILD, metadata)
        try:
            verify_latency_runtime(profile, BUILD, ROOT, allow_diagnostic=diagnostic_only)
        except Exception:
            (BUILD / LATENCY_BUILD).unlink(missing_ok=True)
            raise
    print(
        json.dumps(
            {
                "manifest": str(BUILD / LATENCY_BUILD),
                "cookie": metadata["cookie"],
                "outputs": metadata["outputs"],
                "activation_requested": False,
                "device_proof": False,
                "diagnostic_only": diagnostic_only,
                "diagnostic_zero_ttp": args.diagnostic_zero_ttp,
                "experimental_raw_island": args.experimental_raw_island,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ELFError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
