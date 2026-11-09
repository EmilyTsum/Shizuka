# SPDX-License-Identifier: GPL-2.0-only
"""Build through a reviewed device backend; importing this module never starts a build."""

import json

from shizuka.artifacts import verify_android_build
from shizuka.backends import get_backend
from shizuka.profiles import load_profile
from shizuka.project import BUILD, native_sources_fingerprint


def main():
    profile = load_profile()
    profile.verify_inputs()
    if (BUILD / profile.data["payloads"]["apk"]).is_file():
        verify_android_build(profile)
    get_backend(profile).build(profile)
    (BUILD / "native-build.json").write_text(
        json.dumps(
            {
                "profile_id": profile.id,
                "profile_fingerprint": profile.fingerprint,
                "sources_fingerprint": native_sources_fingerprint(profile.driver),
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
