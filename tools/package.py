# SPDX-License-Identifier: GPL-2.0-only
"""Package only locally built payloads; never copy another device's audio proofs."""

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

from build_settings_app import PACKAGE, VERSION_CODE, settings_sources_fingerprint
from shizuka.artifacts import read_manifest, verify_native_validation
from shizuka.latency_artifacts import (
    EXTRA_OUTPUTS,
    LATENCY_BUILD,
    reject_diagnostic_manifest,
    verify_latency_runtime,
)
from shizuka.profiles import load_profile
from shizuka.validation import validate_evidence

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DIST = ROOT / "dist"

# KernelSU exposes its non-Action module entry only through webroot/index.html.
# These reviewed assets launch settings.sh; no browser settings UI is allowed.
NATIVE_ENTRY_ASSETS = {
    "webroot/index.html": "a1fc1772502e911f535d93a20060b661af90452a6e6ba274e6837016e3ca901b",
    "webroot/launcher.js": "615f3f52bcd425d1e1349a0fdbc7f6183f93a3cacae998db7fecb80f3720bc84",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def validate_native_ui_only(directory):
    seen = set()
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory)
        name = relative.as_posix()
        if name == "webroot" and path.is_dir() and not path.is_symlink():
            continue
        if relative.parts[0] == "webroot" or path.suffix.lower() in {
            ".html",
            ".htm",
            ".css",
            ".js",
        }:
            expected = NATIVE_ENTRY_ASSETS.get(name)
            if (
                expected is None
                or path.is_symlink()
                or not path.is_file()
                or sha(path.read_bytes()) != expected
            ):
                raise ValueError(
                    f"Only native settings UI is supported; unapproved launcher asset: {relative}"
                )
            seen.add(name)
    if seen != NATIVE_ENTRY_ASSETS.keys():
        raise ValueError(
            "KernelSU native launcher is incomplete; restore its reviewed entry assets"
        )


def copy_latency_runtime(metadata, build, stage):
    """Copy the optional inactive runtime without extending core audio proofs."""
    identity = latency_runtime_identity(metadata)
    for source, target in [
        ("latency-provider", "latency-provider"),
        ("patched-libar-gsl.so", "payload/patched-libar-gsl.so"),
    ]:
        if sha((build / source).read_bytes()) != metadata["outputs"][source]:
            raise ValueError("Latency artifact changed while packaging")
        shutil.copy2(build / source, stage / target)
    (stage / "latency-provider.sha256").write_text(metadata["outputs"]["latency-provider"] + "\n")
    shutil.copy2(build / LATENCY_BUILD, stage / LATENCY_BUILD)
    identity_path = stage / "latency-runtime.identity"
    identity_path.write_text(identity, encoding="ascii")
    identity_path.chmod(0o644)


def latency_runtime_identity(metadata):
    """Installed shell identity for the separately verified canonical artifact pair."""
    reject_diagnostic_manifest(metadata)
    try:
        fields = [
            metadata["cookie"],
            metadata["outputs"]["patched-libar-gsl.so"],
            metadata["core"]["outputs"]["native"],
            metadata["core"]["outputs"]["hal"],
        ]
    except (KeyError, TypeError) as error:
        raise ValueError("Incomplete packaged latency runtime identity") from error
    if any(
        not isinstance(value, str)
        or len(value) != length
        or any(c not in "0123456789abcdef" for c in value)
        for value, length in zip(fields, (32, 64, 64, 64), strict=True)
    ) or fields[0] == "0" * 32:
        raise ValueError("Invalid packaged latency runtime identity")
    return "1 " + " ".join(fields) + "\n"


def main(*, with_latency_runtime=False):
    validate_native_ui_only(ROOT / "module")
    profile = load_profile()
    tests = json.loads((ROOT / "evidence/ll-offline-tests.json").read_text())
    validate_evidence(tests, profile)
    manifest = read_manifest(profile, BUILD)
    verify_native_validation(profile, BUILD)
    latency = verify_latency_runtime(profile, BUILD, ROOT) if with_latency_runtime else None
    settings = json.loads((BUILD / "settings-app-build.json").read_text())
    settings_apk = BUILD / "Shizuka-settings.apk"
    if (
        settings.get("schema") != 1
        or settings.get("package") != PACKAGE
        or settings.get("version_code") != VERSION_CODE
        or settings.get("sources_sha256") != settings_sources_fingerprint()
        or settings.get("apk_sha256") != sha(settings_apk.read_bytes())
    ):
        raise ValueError("Settings APK is stale or changed; run tools/build_settings_app.py")
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
            *EXTRA_OUTPUTS,
            LATENCY_BUILD,
            "latency-provider.sha256",
            "latency-runtime.identity",
        ),
    )
    validate_native_ui_only(stage)
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
    if latency is not None:
        copy_latency_runtime(latency, BUILD, stage)
    shutil.copy2(BUILD / "status-dex/classes.dex", stage / "status.dex")
    shutil.copy2(BUILD / "policy-dex/classes.dex", stage / "policy.dex")
    (stage / "policy.sha256").write_text(sha((stage / "policy.dex").read_bytes()) + "\n")
    shutil.copy2(BUILD / "dsp-capture", stage / "dsp-capture")
    (stage / "dsp-capture.sha256").write_text(diagnostic["output_sha256"] + "\n")
    shutil.copy2(settings_apk, stage / "settings.apk")
    (stage / "settings.sha256").write_text(settings["apk_sha256"] + "\n")
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
    suffix = "-latency" if with_latency_runtime else ""
    output = DIST / f"Shizuka-{profile.runtime_id}-{properties['version']}{suffix}.zip"
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(stage))
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise SystemExit("Archive verification failed.")
        if "proofs/verified-payloads.sha256" in archive.namelist():
            raise SystemExit("A new build must not inherit an audio verification proof.")
        if sha(archive.read("settings.apk")) != settings["apk_sha256"]:
            raise SystemExit("Packaged settings APK hash mismatch.")
        for entry in manifest:
            if sha(archive.read("payload/" + entry["output"])) != entry["output_sha256"]:
                raise SystemExit("Packaged payload hash mismatch.")
        if latency is not None:
            if latency != verify_latency_runtime(profile, BUILD, ROOT):
                raise ValueError("Latency runtime identity changed while packaging")
            if (
                sha(archive.read("latency-provider")) != latency["outputs"]["latency-provider"]
                or sha(archive.read("payload/patched-libar-gsl.so"))
                != latency["outputs"]["patched-libar-gsl.so"]
                or archive.read("latency-provider.sha256").decode()
                != latency["outputs"]["latency-provider"] + "\n"
                or json.loads(archive.read(LATENCY_BUILD)) != latency
                or archive.read("latency-runtime.identity").decode()
                != latency_runtime_identity(latency)
            ):
                raise ValueError(
                    "Packaged latency runtime does not match its separate artifact identity"
                )
    print(output)


if __name__ == "__main__":
    try:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument(
            "--with-latency-runtime",
            action="store_true",
            help="include separately built, inactive latency runtime/provider artifacts",
        )
        args = parser.parse_args()
        main(with_latency_runtime=args.with_latency_runtime)
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
