# SPDX-License-Identifier: GPL-2.0-only
"""Test pure Android policy logic; optionally compile every Android helper against an SDK jar."""

import argparse
import json
import os
import subprocess
from pathlib import Path

from shizuka.java import java_tools
from shizuka.project import ANDROID, BUILD, ROOT
from shizuka.tuning import verify_generated
from shizuka.validation import POLICY_CASES


def verify_policies(android_jar=None):
    verify_generated()
    java, compiler = java_tools()
    classes = BUILD / "policy-host-tests"
    classes.mkdir(parents=True, exist_ok=True)
    cases = POLICY_CASES
    sources = [
        ANDROID / (name + ".java")
        for name in [
            "ClassicQhsPolicy",
            "ControllerFeaturesSchema",
            "ExtensionConfig",
            "DevicePreferences",
            "CodecPresentation",
            "ExtensionConfigStore",
            "ProfileSession",
            "ControlJson",
            "TuningCatalog",
            "TuningDefinitions",
            "CodecTuning",
            "DspLogDecoder",
            "DiagnosticStore",
        ]
    ]
    subprocess.run(
        [
            *compiler,
            "--release",
            "8",
            "-d",
            str(classes),
            *map(str, sources),
            *(str(ROOT / "tests" / (name + ".java")) for _, name in cases),
        ],
        check=True,
    )
    results = []
    for key, name in cases:
        output = subprocess.check_output([java, "-cp", str(classes), name], text=True).strip()
        print(output)
        results.append({"test": key, "passed": True, "output": output})
    if android_jar:
        jar = Path(android_jar)
        if not jar.is_file():
            raise ValueError(f"Android API jar is missing: {jar}")
        android_classes = BUILD / "android-compile-check"
        android_classes.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                *compiler,
                "--release",
                "8",
                "-classpath",
                str(jar),
                "-d",
                str(android_classes),
                *map(str, sorted(ANDROID.glob("*.java"))),
                str(ROOT / "tests/BluetoothRuntimeTest.java"),
            ],
            check=True,
        )
        subprocess.run(
            [
                java,
                "-cp",
                os.pathsep.join([str(android_classes), str(jar)]),
                "BluetoothRuntimeTest",
            ],
            check=True,
        )
        # Disabled/default policy must work without a phone or even a configuration file.
        missing = BUILD / "missing-extension-config"
        cli = [
            java,
            "-cp",
            os.pathsep.join([str(android_classes), str(jar)]),
            "BtConfig",
            str(missing / "new.conf"),
            str(missing / "legacy.conf"),
        ]
        for action in ["get", "validate", "watch", "help"]:
            output = subprocess.check_output(
                [*cli, action],
                text=True,
            )
            if action == "watch" and "extension_watch=disabled" not in output:
                raise AssertionError("Default policy unexpectedly started a resident service")
        for command, expected in [
            (["device", "active", "transport", "output", "invalid"], "invalid transport"),
            (["device", "not-an-address", "get"], "invalid Bluetooth address"),
            (["device", "active", "profile", "unknown", "true"], "invalid profile"),
            (["set", "target_address", "AA:BB:CC:DD:EE:FF"], "only extension settings"),
            (["set", "adaptive_priority", "-1"], "invalid adaptive_priority"),
        ]:
            result = subprocess.run([*cli, *command], capture_output=True, text=True)
            if result.returncode != 1 or expected not in result.stderr or "Stub!" in result.stderr:
                raise AssertionError("Invalid command reached Android services: " + result.stderr)
        print(
            "Android helpers compiled; default configuration and disabled watcher run without Android services."
        )
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--android-jar", help="Optional Android 35 public android.jar for full source compilation"
    )
    args = parser.parse_args()
    results = verify_policies(args.android_jar)
    evidence = ROOT / "evidence"
    evidence.mkdir(exist_ok=True)
    (evidence / "policy-host-tests.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
