# SPDX-License-Identifier: GPL-2.0-only
"""Run host regressions; full mode also rebuilds the selected native backend."""

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from shizuka.artifacts import (
    NATIVE_VALIDATION,
    native_build_snapshot,
    record_native_validation,
    write_metadata,
)
from shizuka.backends import get_backend
from shizuka.profiles import load_profile
from shizuka.project import BUILD, ROOT, native_source, offline_sources_fingerprint
from shizuka.validation import PYTHON_CASES, validate_evidence
from verify_policies import verify_policies


def verify_native(profile):
    backend = get_backend(profile)
    compiler = (
        shlex.split(os.environ["CXX"])
        if os.environ.get("CXX")
        else [shutil.which("clang++") or shutil.which("g++") or "clang++"]
    )
    base = compiler + [
        "-I" + str(ROOT / "src"),
        "-std=c++17",
        "-g",
        "-fsanitize=address,undefined",
        *backend.host_flags(),
    ]
    jdk = Path(os.environ.get("JAVA_HOME", ""))
    jni_root = Path(os.environ.get("SHIZUKA_JNI_INCLUDE", str(jdk / "include")))
    if getattr(backend, "HOST_REQUIRES_JNI", False) and not (jni_root / "jni.h").is_file():
        raise ValueError("Set JAVA_HOME to a JDK, or SHIZUKA_JNI_INCLUDE to JNI headers")
    jni = [
        "-I" + str(jni_root),
        "-I" + str(jni_root / ("darwin" if sys.platform == "darwin" else "linux")),
    ]
    results = []
    for name, files, flags in backend.host_cases(jni):
        extra = (
            [str(ROOT / "tests/ll_native_tramp_stubs.cpp")] if "aptx_ll_native.cpp" in files else []
        )
        test = ROOT / "tests" / f"{name}_test.cpp"
        output = BUILD / f"host-{name}-test"
        subprocess.run(
            base
            + flags
            + extra
            + [str(test)]
            + [str(native_source(profile.driver, name)) for name in files]
            + ["-o", str(output)],
            check=True,
        )
        result = subprocess.check_output([str(output)], text=True).strip()
        print(name + ": " + result)
        results.append(
            {
                "test": name,
                "passed": True,
                "output": result,
                "sanitizers": ["address", "undefined"],
                "leak_detection_disabled": "detect_leaks=0" in os.environ.get("ASAN_OPTIONS", ""),
            }
        )
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host-only", action="store_true", help="Do not rebuild proprietary native inputs"
    )
    parser.add_argument("--android-jar", help="Also compile Android helpers against this API jar")
    args = parser.parse_args()
    profile = load_profile()
    BUILD.mkdir(exist_ok=True)
    evidence = ROOT / "evidence"
    host_evidence = evidence / "ll-offline-tests.json"
    host_evidence.unlink(missing_ok=True)
    if not args.host_only:
        (BUILD / NATIVE_VALIDATION).unlink(missing_ok=True)
    fingerprint = offline_sources_fingerprint()
    results = verify_native(profile)
    results.extend(verify_policies(args.android_jar))
    for name, filename in PYTHON_CASES:
        run = subprocess.run(
            [sys.executable, str(ROOT / "tests" / filename)],
            capture_output=True,
            text=True,
        )
        output = (run.stdout + run.stderr).strip()
        print(output, flush=True)
        run.check_returncode()
        results.append({"test": name, "passed": True, "output": output})
    if fingerprint != offline_sources_fingerprint():
        raise ValueError("Sources changed during verification; rerun the checks")
    for result in results:
        result.update(
            profile_id=profile.id,
            profile_fingerprint=profile.fingerprint,
            sources_fingerprint=fingerprint,
        )
    validate_evidence(results, profile)
    if not args.host_only:
        subprocess.run([sys.executable, str(ROOT / "tools/build.py")], check=True)
        builds = native_build_snapshot(profile)
        get_backend(profile).verify_dispatch(profile)
        if fingerprint != offline_sources_fingerprint():
            raise ValueError("Sources changed during integration; rerun the checks")
        record_native_validation(profile, builds)
    evidence.mkdir(exist_ok=True)
    write_metadata(host_evidence, results)


if __name__ == "__main__":
    main()
