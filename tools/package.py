# SPDX-License-Identifier: GPL-2.0-only
"""Package only locally built payloads; never copy another device's audio proofs."""

import hashlib
import json
import shutil
import zipfile
from pathlib import Path

from shizuka.artifacts import read_manifest, verify_native_validation
from shizuka.profiles import load_profile
from shizuka.validation import validate_evidence

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DIST = ROOT / "dist"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    profile = load_profile()
    tests = json.loads((ROOT / "evidence/ll-offline-tests.json").read_text())
    validate_evidence(tests, profile)
    manifest = read_manifest(profile, BUILD)
    verify_native_validation(profile, BUILD)
    stage = BUILD / "package"
    diagnostic = json.loads((BUILD / "diagnostics-build.json").read_text())
    if diagnostic != {
        "source_sha256": sha((ROOT / "src/diagnostics/dsp_capture.c").read_bytes()),
        "builder_sha256": sha((ROOT / "tools/build_diagnostics.py").read_bytes()),
        "output_sha256": sha((BUILD / "dsp-capture").read_bytes()),
    }:
        raise ValueError("Diagnostic reader build is stale; rebuild Android artifacts")
    shutil.rmtree(stage, ignore_errors=True)
    shutil.copytree(
        ROOT / "module",
        stage,
        ignore=shutil.ignore_patterns(
            "proofs",
            "state",
            "payload",
            "*.dex",
            "payloads.sha256",
            "policy.sha256",
            ".no-auto",
            "disable",
            "remove",
            ".enable-*",
            "telemetry.json",
            "diagnostics",
            "*.jsonl",
        ),
    )
    (stage / "device-profile").write_text(profile.runtime_id + "\n")
    (stage / "inputs.sha256").write_text(profile.input_manifest())
    shutil.copy2(ROOT / "LICENSE", stage / "LICENSE")
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
    shutil.copy2(BUILD / "dsp-capture", stage / "dsp-capture")
    (stage / "dsp-capture.sha256").write_text(diagnostic["output_sha256"] + "\n")
    for src, name in [
        (ROOT / "config/config.example.conf", "config.example.conf"),
        (ROOT / "config/CONFIG-GUIDE.txt", "CONFIG-GUIDE.txt"),
    ]:
        shutil.copy2(src, stage / name)
    DIST.mkdir(exist_ok=True)
    properties = dict(
        line.split("=", 1)
        for line in (stage / "module.prop").read_text().splitlines()
        if "=" in line
    )
    output = DIST / f"Shizuka-{profile.runtime_id}-{properties['version']}.zip"
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
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
