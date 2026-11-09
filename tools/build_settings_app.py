#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the native OneUIProject settings companion and bind it to its sources."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "tools/android-app"
BUILD = ROOT / "build"
VERSION_CODE = 17
VERSION_NAME = "0.10.0-test"
PACKAGE = "dev.shizuka.settings"


def settings_sources_fingerprint():
    """Stable fingerprint independent of machine-local Gradle state/signing keys."""
    files = [Path(__file__).resolve()]
    files.extend(path for path in APP.rglob("*") if path.is_file()
                 and not {".gradle", "build", ".idea"}.intersection(path.relative_to(APP).parts)
                 and path.name not in {"local.properties", ".DS_Store"}
                 and path.suffix not in {".jks", ".keystore"})
    result = hashlib.sha256()
    for path in sorted(files):
        result.update(path.relative_to(ROOT).as_posix().encode() + b"\0")
        result.update(path.read_bytes() + b"\0")
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME"))
    parser.add_argument("--gradle-user-home", default=os.environ.get("GRADLE_USER_HOME"))
    args = parser.parse_args()
    if not args.sdk:
        raise SystemExit("Set ANDROID_SDK_ROOT or pass --sdk.")
    sdk = Path(args.sdk).expanduser().resolve()
    tools = sdk / "build-tools/35.0.0"
    for file in (tools / "apksigner", sdk / "platforms/android-35/android.jar"):
        if not file.is_file():
            raise SystemExit(f"Required Android SDK file is missing: {file}")
    BUILD.mkdir(exist_ok=True)
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    if args.gradle_user_home:
        env["GRADLE_USER_HOME"] = str(Path(args.gradle_user_home).expanduser().resolve())
    before = settings_sources_fingerprint()
    subprocess.run([str(APP / "gradlew"), "--no-daemon", "--console=plain", "assembleRelease"], cwd=APP, env=env, check=True)
    unsigned = BUILD / "settings-app-gradle/outputs/apk/release/android-app-release-unsigned.apk"
    if not unsigned.is_file():
        candidates = list((BUILD / "settings-app-gradle/outputs/apk/release").glob("*-release-unsigned.apk"))
        if len(candidates) != 1:
            raise SystemExit("Expected one unsigned settings APK.")
        unsigned = candidates[0]
    key = Path(env.get("SHIZUKA_SETTINGS_KEYSTORE", str(BUILD / "settings-signing/shizuka-settings.jks"))).expanduser()
    key.parent.mkdir(parents=True, exist_ok=True)
    java_home = env.get("JAVA_HOME")
    keytool = str(Path(java_home) / "bin/keytool") if java_home else shutil.which("keytool")
    if not keytool:
        raise SystemExit("A JDK with keytool is required.")
    if not key.exists():
        subprocess.run([keytool, "-genkeypair", "-keystore", str(key), "-storepass", "shizuka-local-build", "-keypass", "shizuka-local-build", "-alias", "settings", "-keyalg", "RSA", "-keysize", "3072", "-validity", "10000", "-dname", "CN=Shizuka Settings Local Build"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key.chmod(0o600)
    output = BUILD / "Shizuka-settings.apk"
    subprocess.run([str(tools / "apksigner"), "sign", "--ks", str(key), "--ks-key-alias", "settings", "--ks-pass", "pass:shizuka-local-build", "--key-pass", "pass:shizuka-local-build", "--out", str(output), str(unsigned)], check=True, env=env)
    subprocess.run([str(tools / "apksigner"), "verify", "--verbose", str(output)], check=True, env=env)
    after = settings_sources_fingerprint()
    if before != after:
        raise SystemExit("Settings sources changed during the build. Rebuild the companion.")
    record = {"schema": 1, "package": PACKAGE, "activity": PACKAGE + ".MainActivity", "sources_sha256": after,
              "apk_sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "version_code": VERSION_CODE, "version_name": VERSION_NAME,
              "oneui_design": "1.2.6", "sesl_appcompat": "1.4.0", "sesl_preference": "1.1.0", "sesl_material": "1.5.0"}
    (BUILD / "settings-app-build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"apk": str(output), **record}, indent=2))


if __name__ == "__main__":
    main()
