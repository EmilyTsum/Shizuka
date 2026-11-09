# SPDX-License-Identifier: GPL-2.0-only
"""Project paths and validated build options, without import-time build work."""

import hashlib
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
ANDROID = ROOT / "tools/android"


def option(name, default=None, *, legacy=None):
    return os.environ.get("SHIZUKA_" + name, os.environ.get(legacy, default) if legacy else default)


def inputs_dir():
    return Path(option("INPUTS", str(ROOT / "inputs"), legacy="S25_BASE_BIN")).expanduser()


def digest(path):
    hasher = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def source_fingerprint(paths):
    hasher = hashlib.sha256()
    for path in sorted(map(Path, paths)):
        hasher.update(path.relative_to(ROOT).as_posix().encode() + b"\0")
        hasher.update(path.read_bytes() + b"\0")
    return hasher.hexdigest()


def android_sources_fingerprint():
    return source_fingerprint(
        [
            *ANDROID.glob("*.java"),
            ROOT / "tools/build_android.py",
            ROOT / "config/tuning.json",
            ROOT / "tools/shizuka/tuning.py",
        ]
    )


def native_sources_fingerprint(driver):
    from .backends import BACKENDS

    return source_fingerprint(
        [
            *(path for path in (ROOT / "src").rglob("*") if path.suffix in {".cpp", ".h", ".ld"}),
            ROOT / "tools/shizuka/backends" / (BACKENDS[driver].module + ".py"),
            ROOT / "tools/shizuka/backends/__init__.py",
            ROOT / "tools/build.py",
            ROOT / "tools/shizuka/project.py",
            ROOT / "tools/shizuka/profiles.py",
        ]
    )


def native_source(driver, name):
    shared = ROOT / "src" / name
    specific = ROOT / "src/backends" / driver.replace("-", "_") / name
    selected = specific if specific.is_file() else shared
    if not selected.is_file():
        raise ValueError(f"Source is unavailable for backend {driver}: {name}")
    return selected


def offline_sources_fingerprint():
    sources = [
        *(path for path in (ROOT / "src").rglob("*") if path.suffix in {".c", ".cpp", ".h", ".ld"}),
        # The companion has its own source-bound APK build. A screen-only edit
        # must not invalidate native encoder tests or trigger a Bluetooth rebuild.
        *(
            path for path in (ROOT / "tools").rglob("*")
            if path.suffix in {".py", ".java"}
            and not path.is_relative_to(ROOT / "tools/android-app")
            and path != ROOT / "tools/build_settings_app.py"
        ),
        *(
            path
            for path in (ROOT / "tests").rglob("*")
            if path.suffix in {".py", ".java", ".c", ".cpp", ".cjs"}
        ),
        *(
            path
            for path in (ROOT / "module").rglob("*")
            if path.suffix in {".sh", ".js", ".prop"}
            and not set(path.relative_to(ROOT / "module").parts) & {"state", "payload", "proofs"}
        ),
        ROOT / "config/config.example.conf",
        ROOT / "config/tuning.json",
    ]
    return source_fingerprint(sources)


def ndk_bin():
    configured = option("NDK_BIN", legacy="S25_NDK_BIN")
    if not configured:
        raise ValueError("Set SHIZUKA_NDK_BIN to the Android NDK LLVM prebuilt bin directory.")
    directory = Path(configured).expanduser()
    if not (directory / "clang++").is_file():
        raise ValueError(f"clang++ was not found in {directory}")
    return directory


def native_options():
    choices = {
        "RATE": ("96000", {44100, 48000, 96000}),
        "MODE": ("4096", {4096, 8192, 12288}),
        "R2": ("3", {0, 1, 2, 3}),
    }
    result = {}
    for key, (default, allowed) in choices.items():
        raw = option(key, default, legacy="S25_" + key)
        if not raw.isdecimal() or int(raw) not in allowed:
            raise ValueError(f"Invalid SHIZUKA_{key}: {raw}")
        result[key] = int(raw)
    return result
