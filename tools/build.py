# SPDX-License-Identifier: GPL-2.0-only
"""Build through a reviewed device backend; importing this module never starts a build."""

from shizuka.artifacts import (
    NATIVE_BUILD,
    NATIVE_VALIDATION,
    record_native_build,
    verify_android_build,
)
from shizuka.backends import get_backend
from shizuka.profiles import load_profile
from shizuka.project import BUILD, native_sources_fingerprint


def main():
    profile = load_profile()
    for name in (NATIVE_BUILD, NATIVE_VALIDATION):
        (BUILD / name).unlink(missing_ok=True)
    profile.verify_inputs()
    if (BUILD / profile.data["payloads"]["apk"]).is_file():
        verify_android_build(profile)
    before = native_sources_fingerprint(profile.driver)
    options = get_backend(profile).build(profile)
    if before != native_sources_fingerprint(profile.driver):
        raise ValueError("Sources changed during the native build; rebuild artifacts")
    record_native_build(profile, options)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
