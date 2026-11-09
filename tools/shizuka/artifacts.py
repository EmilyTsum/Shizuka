# SPDX-License-Identifier: GPL-2.0-only
"""Refuse stale or mixed-profile artifacts before patching or packaging."""

import json

from .project import BUILD, android_sources_fingerprint, digest, native_sources_fingerprint


def _read(build, name, profile, sources):
    path = build / name
    if not path.is_file():
        raise ValueError(f"Missing {name}; rebuild artifacts for {profile.id}")
    value = json.loads(path.read_text())
    if (
        value.get("profile_id"),
        value.get("profile_fingerprint"),
        value.get("sources_fingerprint"),
    ) != (profile.id, profile.fingerprint, sources):
        raise ValueError(f"Stale or mixed-profile {name}; rebuild artifacts")
    return value


def verify_android_build(profile, build=BUILD):
    metadata = _read(build, "android-build.json", profile, android_sources_fingerprint())
    if metadata.get("apk_patches_sha256") != digest(profile.apk_patches):
        raise ValueError("APK patch definitions changed; rebuild Android artifacts")
    paths = {
        "apk": profile.data["payloads"]["apk"],
        "policy": "policy-dex/classes.dex",
        "status": "status-dex/classes.dex",
    }
    for name, path in paths.items():
        if metadata.get("outputs", {}).get(name) != digest(build / path):
            raise ValueError(f"Android build output changed: {path}")


def verify_native_build(profile, build=BUILD):
    _read(build, "native-build.json", profile, native_sources_fingerprint(profile.driver))
