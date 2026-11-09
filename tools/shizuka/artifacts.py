# SPDX-License-Identifier: GPL-2.0-only
"""Refuse stale or mixed-profile artifacts before patching or packaging."""

import json
import os
import tempfile

from .project import (
    BUILD,
    android_sources_fingerprint,
    digest,
    native_sources_fingerprint,
    offline_sources_fingerprint,
)

NATIVE_BUILD = "native-build.json"
NATIVE_VALIDATION = "native-validation.json"


def _identity(profile, sources):
    return {
        "schema": 1,
        "profile_id": profile.id,
        "profile_fingerprint": profile.fingerprint,
        "sources_fingerprint": sources,
    }


def write_metadata(path, metadata):
    """Publish a completed result atomically; a partial JSON file is never evidence."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
            temporary = stream.name
            json.dump(metadata, stream, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary and os.path.exists(temporary):
            os.unlink(temporary)


def _read(build, name, profile, sources):
    path = build / name
    advice = "run tools/verify_offline.py" if name == NATIVE_VALIDATION else "rebuild artifacts"
    if not path.is_file():
        raise ValueError(f"Missing {name}; {advice} for {profile.id}")
    value = json.loads(path.read_text())
    if not isinstance(value, dict) or any(
        value.get(key) != expected for key, expected in _identity(profile, sources).items()
    ):
        raise ValueError(f"Stale or mixed-profile {name}; {advice}")
    return value


def read_manifest(profile, build=BUILD, *, complete=True):
    manifest = json.loads((build / "patch-manifest.json").read_text())
    if not isinstance(manifest, list) or not all(isinstance(entry, dict) for entry in manifest):
        raise ValueError("Invalid payload manifest; rebuild artifacts")
    expected = set(profile.data["payloads"].values())
    outputs = [entry.get("output") for entry in manifest]
    required = expected if complete else expected - {profile.data["payloads"]["apk"]}
    if (
        not all(isinstance(name, str) for name in outputs)
        or len(outputs) != len(set(outputs))
        or not required.issubset(outputs)
        or not set(outputs).issubset(expected)
    ):
        raise ValueError("Payload outputs are missing, duplicated or unexpected; rebuild artifacts")
    for entry in manifest:
        if (
            entry.get("profile_id") != profile.id
            or entry.get("profile_fingerprint") != profile.fingerprint
            or entry.get("output_sha256") != digest(build / entry["output"])
        ):
            raise ValueError("Payload manifest does not match the selected profile/build")
    return manifest


def verify_android_build(profile, build=BUILD):
    metadata = _read(build, "android-build.json", profile, android_sources_fingerprint())
    if metadata.get("apk_patches_sha256") != digest(profile.apk_patches):
        raise ValueError("APK patch definitions changed; rebuild Android artifacts")
    paths = {
        "apk": profile.data["payloads"]["apk"],
        "policy": "policy-dex/classes.dex",
        "status": "status-dex/classes.dex",
    }
    outputs = {name: digest(build / path) for name, path in paths.items()}
    if outputs["policy"] != outputs["status"]:
        raise ValueError("Shared Android tool libraries differ; rebuild Android artifacts")
    if metadata.get("outputs") != outputs:
        raise ValueError(
            "Android outputs do not match their build record; rebuild Android artifacts"
        )


def verify_native_build(profile, build=BUILD):
    metadata = _read(build, NATIVE_BUILD, profile, native_sources_fingerprint(profile.driver))
    read_manifest(profile, build, complete=False)
    if metadata.get("manifest_sha256") != digest(build / "patch-manifest.json"):
        raise ValueError("Payload manifest changed after the native build; rebuild artifacts")
    outputs = {
        role: digest(build / name)
        for role, name in profile.data["payloads"].items()
        if role != "apk"
    }
    if metadata.get("outputs") != outputs or not isinstance(metadata.get("options"), dict):
        raise ValueError("Native output/options do not match their build record")


def record_native_build(profile, options, build=BUILD):
    if not isinstance(options, dict):
        raise ValueError("The native backend must return its build options as a dictionary")
    read_manifest(profile, build, complete=False)
    metadata = _identity(profile, native_sources_fingerprint(profile.driver))
    metadata.update(
        options=options,
        manifest_sha256=digest(build / "patch-manifest.json"),
        outputs={
            role: digest(build / name)
            for role, name in profile.data["payloads"].items()
            if role != "apk"
        },
    )
    write_metadata(build / NATIVE_BUILD, metadata)


def _validated_builds(build):
    return {name: digest(build / name) for name in (NATIVE_BUILD, "android-build.json")}


def native_build_snapshot(profile, build=BUILD):
    before = _validated_builds(build)
    verify_android_build(profile, build)
    verify_native_build(profile, build)
    read_manifest(profile, build)
    if before != _validated_builds(build):
        raise ValueError("Build records changed while validating artifacts; rerun verification")
    return before


def record_native_validation(profile, verified_builds, build=BUILD):
    """Bind a successful dispatch check to the same build records captured before it."""
    if verified_builds != native_build_snapshot(profile, build):
        raise ValueError("Artifacts changed during native integration; rerun verification")
    metadata = _identity(profile, offline_sources_fingerprint())
    metadata.update(passed=True, builds=verified_builds)
    write_metadata(build / NATIVE_VALIDATION, metadata)


def verify_native_validation(profile, build=BUILD):
    metadata = _read(build, NATIVE_VALIDATION, profile, offline_sources_fingerprint())
    if metadata.get("passed") is not True or metadata.get("builds") != native_build_snapshot(
        profile, build
    ):
        raise ValueError(
            "Native integration is unverified for these artifacts; run verify_offline.py"
        )
