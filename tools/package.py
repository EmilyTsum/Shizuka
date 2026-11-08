# SPDX-License-Identifier: GPL-2.0-only
"""Package only locally built payloads; never copy another device's audio proofs."""

import hashlib
import json
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DIST = ROOT / "dist"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    tests = json.loads((ROOT / "evidence/ll-offline-tests.json").read_text())
    required_tests = {
        "ll_switch",
        "aptx_ll",
        "ll_native_protocol",
        "ll_ui_registration",
        "protocol",
        "peer_negotiation",
        "vendor",
        "ui_callback",
        "layout_stability",
        "classic_qhs_policy",
    }
    if (
        len(tests) != len(required_tests)
        or {test["test"] for test in tests} != required_tests
        or not all(test["passed"] for test in tests)
    ):
        raise SystemExit("Run tools/verify_offline.py before packaging.")
    manifest = json.loads((BUILD / "patch-manifest.json").read_text())
    expected = {
        "patched-libbluetooth_jni.so",
        "patched-libsehbluetooth_audio_session_aidl.so",
        "patched-Bluetooth.apk",
        "runtime.prop",
    }
    if {entry["output"] for entry in manifest} != expected:
        raise SystemExit("Native, HAL, APK and runtime property outputs are required.")
    stage = BUILD / "package"
    shutil.rmtree(stage, ignore_errors=True)
    shutil.copytree(
        ROOT / "module",
        stage,
        ignore=shutil.ignore_patterns("proofs", "*.dex", "payloads.sha256", "policy.sha256"),
    )
    (stage / "payload").mkdir()
    (stage / "proofs").mkdir()
    entries = []
    for entry in manifest:
        path = BUILD / entry["output"]
        if sha(path.read_bytes()) != entry["output_sha256"]:
            raise SystemExit(f"Build output changed after verification: {path.name}")
        shutil.copy2(path, stage / "payload" / path.name)
        entries.append(f"{entry['output_sha256']} {path.name}\n")
    hashes = "".join(entries)
    (stage / "payloads.sha256").write_text(hashes)
    (stage / "proofs/offline-ready").write_text(sha(hashes.encode()) + "\n")
    shutil.copy2(BUILD / "status-dex/classes.dex", stage / "status.dex")
    shutil.copy2(BUILD / "policy-dex/classes.dex", stage / "policy.dex")
    (stage / "policy.sha256").write_text(sha((stage / "policy.dex").read_bytes()) + "\n")
    for src, name in [
        (ROOT / "config/config.example.conf", "config.example.conf"),
        (ROOT / "config/CONFIG-GUIDE.txt", "CONFIG-GUIDE.txt"),
    ]:
        shutil.copy2(src, stage / name)
    DIST.mkdir(exist_ok=True)
    output = DIST / "Shizuka-DZIF-0.6.0-test.zip"
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(stage))
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise SystemExit("Archive verification failed.")
        if "proofs/verified-payloads.sha256" in archive.namelist():
            raise SystemExit("A new build must not inherit an audio verification proof.")
        for entry in manifest:
            if sha(archive.read("payload/" + entry["output"])) != entry["output_sha256"]:
                raise SystemExit("Packaged payload hash mismatch.")
    print(output)


if __name__ == "__main__":
    main()
