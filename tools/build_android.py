# SPDX-License-Identifier: GPL-2.0-only
"""Rebuild the Bluetooth APK edits and external policy from local stock inputs."""

import hashlib
import json
import os
import shutil
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
INPUTS = Path(os.environ.get("S25_BASE_BIN", ROOT / "inputs"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sdk_paths():
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk:
        raise SystemExit(
            "Set ANDROID_SDK_ROOT to an Android SDK with platform 35 and build-tools 35.0.0."
        )
    sdk = Path(sdk).expanduser()
    tools = sdk / "build-tools" / os.environ.get("S25_BUILD_TOOLS", "35.0.0")
    android = sdk / "platforms/android-35/android.jar"
    for path in [tools / "d8", tools / "zipalign", android]:
        if not path.is_file():
            raise SystemExit(f"Required Android SDK file is missing: {path}")
    return tools, android


def check_inputs():
    profile = json.loads((ROOT / "profile.json").read_text())
    for target in profile["targets"]:
        path = INPUTS / target["local_file"]
        if not path.is_file() or digest(path) != target["sha256"]:
            raise SystemExit(
                f"Stock input is missing or does not match the DZIF profile: {path.name}"
            )


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
    edits = json.loads((ROOT / "patches/bluetooth-apk.json").read_text())
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
    with (
        zipfile.ZipFile(INPUTS / "Bluetooth.apk") as before,
        zipfile.ZipFile(output) as after,
    ):
        for name in ["AndroidManifest.xml", "resources.arsc"]:
            if before.read(name) != after.read(name):
                raise SystemExit(f"APK build unexpectedly changed {name}")
    return output


def build_policy(tools, android):
    java_home = os.environ.get("JAVA_HOME")
    if not java_home:
        raise SystemExit("Set JAVA_HOME to a JDK (17 or newer).")
    javac = Path(java_home) / "bin/javac"
    classes = BUILD / "policy-classes"
    dex = BUILD / "policy-dex"
    shutil.rmtree(classes, ignore_errors=True)
    shutil.rmtree(dex, ignore_errors=True)
    classes.mkdir(parents=True)
    dex.mkdir(parents=True)
    subprocess.run(
        [
            str(javac),
            "--release",
            "8",
            "-classpath",
            str(android),
            "-d",
            str(classes),
            str(ROOT / "tools/BtConfig.java"),
            str(ROOT / "tools/ClassicQhsPolicy.java"),
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
    java_home = os.environ.get("JAVA_HOME")
    if not java_home:
        raise SystemExit("Set JAVA_HOME to a JDK (17 or newer).")
    classes = BUILD / "status-classes"
    dex = BUILD / "status-dex"
    shutil.rmtree(classes, ignore_errors=True)
    shutil.rmtree(dex, ignore_errors=True)
    classes.mkdir(parents=True)
    dex.mkdir(parents=True)
    subprocess.run(
        [
            str(Path(java_home) / "bin/javac"),
            "--release",
            "8",
            "-classpath",
            str(android),
            "-d",
            str(classes),
            str(ROOT / "tools/BtStatus.java"),
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


def main():
    check_inputs()
    tools, android = sdk_paths()
    BUILD.mkdir(parents=True, exist_ok=True)
    build_apk(tools)
    build_policy(tools, android)
    build_status(tools, android)


if __name__ == "__main__":
    main()
