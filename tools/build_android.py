# SPDX-License-Identifier: GPL-2.0-only
"""Rebuild the Bluetooth APK edits and external policy from local stock inputs."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from shizuka.artifacts import NATIVE_VALIDATION, write_metadata
from shizuka.java import java_tools
from shizuka.profiles import load_profile
from shizuka.project import ANDROID, android_sources_fingerprint, digest, inputs_dir, option
from shizuka.tuning import verify_generated

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
INPUTS = inputs_dir()


def sdk_paths(require_apk=True):
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk:
        raise SystemExit(
            "Set ANDROID_SDK_ROOT to an Android SDK with platform 35 and build-tools 35.0.0."
        )
    sdk = Path(sdk).expanduser()
    tools = sdk / "build-tools" / option("BUILD_TOOLS", "35.0.0", legacy="S25_BUILD_TOOLS")
    android = sdk / "platforms/android-35/android.jar"
    required = [tools / "d8", android]
    if require_apk:
        required.append(tools / "zipalign")
    for path in required:
        if not path.is_file():
            raise SystemExit(f"Required Android SDK file is missing: {path}")
    return tools, android


def check_inputs():
    load_profile().verify_inputs(INPUTS)


def build_apk(tools):
    apktool = os.environ.get("APKTOOL") or shutil.which("apktool")
    if not apktool:
        raise SystemExit("apktool 3.0.3 is required on PATH (or set APKTOOL).")
    version = subprocess.check_output([apktool, "--version"], text=True).strip()
    if version != "3.0.3":
        raise SystemExit(f"This smali patch was verified with apktool 3.0.3; found {version}.")
    decoded = BUILD / "bluetooth-apk"
    shutil.rmtree(decoded, ignore_errors=True)
    subprocess.run(
        [apktool, "d", "-r", "-o", str(decoded), str(INPUTS / "Bluetooth.apk")],
        check=True,
    )
    edits = json.loads(load_profile().apk_patches.read_text())
    for patch in edits["files"]:
        path = decoded / patch["path"]
        if digest(path) != patch["before_sha256"]:
            raise SystemExit(f"Decoded class does not match the reviewed input: {patch['path']}")
        lines = path.read_text().splitlines(keepends=True)
        for edit in reversed(patch["edits"]):
            start = edit["line"]
            lines[start : start + edit["delete"]] = edit["insert"].splitlines(keepends=True)
        path.write_text("".join(lines))
        if digest(path) != patch["after_sha256"]:
            raise SystemExit(f"APK patch output mismatch: {patch['path']}")
    raw = BUILD / "Bluetooth-unaligned.apk"
    output = BUILD / "patched-Bluetooth.apk"
    subprocess.run([apktool, "b", "--copy-original", "-o", str(raw), str(decoded)], check=True)
    subprocess.run(
        [str(tools / "zipalign"), "-P", "16", "-f", "4", str(raw), str(output)],
        check=True,
    )
    subprocess.run([str(tools / "zipalign"), "-c", "-P", "16", "4", str(output)], check=True)
    subprocess.run([sys.executable, str(ROOT / "tests/apk_offload_hq_check.py")], check=True)
    subprocess.run([sys.executable, str(ROOT / "tests/apk_le_route_check.py")], check=True)
    subprocess.run([sys.executable, str(ROOT / "tests/apk_handoff_check.py")], check=True)
    with (
        zipfile.ZipFile(INPUTS / "Bluetooth.apk") as before,
        zipfile.ZipFile(output) as after,
    ):
        for name in ["AndroidManifest.xml", "resources.arsc"]:
            if before.read(name) != after.read(name):
                raise SystemExit(f"APK build unexpectedly changed {name}")
    return output


def build_policy(tools, android):
    _, javac = java_tools()
    classes = BUILD / "policy-classes"
    dex = BUILD / "policy-dex"
    shutil.rmtree(classes, ignore_errors=True)
    shutil.rmtree(dex, ignore_errors=True)
    classes.mkdir(parents=True)
    dex.mkdir(parents=True)
    subprocess.run(
        [
            *javac,
            "--release",
            "8",
            "-classpath",
            str(android),
            "-d",
            str(classes),
            *map(str, sorted(ANDROID.glob("*.java"))),
        ],
        check=True,
    )
    subprocess.run(
        [
            str(tools / "d8"),
            "--min-api",
            "34",
            "--lib",
            str(android),
            "--output",
            str(dex),
            *map(str, sorted(classes.glob("*.class"))),
        ],
        check=True,
    )
    return dex / "classes.dex"


def build_status(tools, android):
    # One tool library for status, controls and diagnostics; keep both legacy
    # artifact names so existing lifecycle scripts and upgrade paths remain valid.
    dex = BUILD / "status-dex"
    shutil.rmtree(dex, ignore_errors=True)
    dex.mkdir(parents=True)
    shutil.copy2(BUILD / "policy-dex/classes.dex", dex / "classes.dex")
    return dex / "classes.dex"


def main():
    verify_generated()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--helpers-only",
        action="store_true",
        help="Build portable Android settings/status tools without stock APKs or native patches",
    )
    args = parser.parse_args()
    if not args.helpers_only:
        for name in ("android-build.json", NATIVE_VALIDATION):
            (BUILD / name).unlink(missing_ok=True)
        check_inputs()
    tools, android = sdk_paths(require_apk=not args.helpers_only)
    BUILD.mkdir(parents=True, exist_ok=True)
    if not args.helpers_only:
        build_apk(tools)
    build_policy(tools, android)
    build_status(tools, android)
    if args.helpers_only:
        bundle = BUILD / "android-tools"
        shutil.rmtree(bundle, ignore_errors=True)
        (bundle / "lib").mkdir(parents=True)
        for path in [
            "config.sh",
            "playback.sh",
            "status.sh",
            "diagnostics.sh",
            "lib/config.sh",
            "lib/runtime.sh",
            "lib/policy.sh",
        ]:
            shutil.copy2(ROOT / "module" / path, bundle / path)
        shutil.copy2(BUILD / "policy-dex/classes.dex", bundle / "policy.dex")
        shutil.copy2(BUILD / "status-dex/classes.dex", bundle / "status.dex")
        shutil.copy2(ROOT / "LICENSE", bundle / "LICENSE")
        (bundle / "policy.sha256").write_text(digest(bundle / "policy.dex") + "\n")
        (bundle / "README.txt").write_text(
            "Shizuka Android tools (Android 14+ / KernelSU root).\nRun config.sh device active get or status.sh.\nThis folder is a command-line tool bundle, not a KernelSU installation ZIP.\nNative patch activation and vendor controller extensions require a verified device backend.\n"
        )
        print(bundle)
        return
    from build_diagnostics import build_diagnostics

    build_diagnostics()
    profile = load_profile()
    write_metadata(
        BUILD / "android-build.json",
        {
            "schema": 1,
            "profile_id": profile.id,
            "profile_fingerprint": profile.fingerprint,
            "sources_fingerprint": android_sources_fingerprint(),
            "apk_patches_sha256": digest(profile.apk_patches),
            "outputs": {
                name: digest(BUILD / path)
                for name, path in {
                    "apk": "patched-Bluetooth.apk",
                    "policy": "policy-dex/classes.dex",
                    "status": "status-dex/classes.dex",
                }.items()
            },
        },
    )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
