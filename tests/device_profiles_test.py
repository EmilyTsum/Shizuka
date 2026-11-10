# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: E402
"""Profile and artifact boundaries without proprietary inputs or device access."""

import contextlib
import copy
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.artifacts import (
    native_build_snapshot,
    record_native_build,
    record_native_validation,
    verify_android_build,
    verify_native_build,
    verify_native_validation,
)  # noqa: E402
from shizuka.profiles import DeviceProfile, load_profile  # noqa: E402
from shizuka.project import (
    android_sources_fingerprint,
    digest,
    inputs_dir,
    native_options,
    offline_sources_fingerprint,
)  # noqa: E402
from shizuka.validation import required_tests, validate_evidence


class Profiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.d = pathlib.Path(self.tmp.name)
        self.profile = load_profile("samsung-sm-s931z-dzif")

    def selected(self, data):
        path = self.d / "profile.json"
        path.write_text(json.dumps(data))
        return load_profile(path)

    def test_default_profile_matches_runtime_paths_payloads_and_stock_manifest(self):
        profile = self.profile
        self.assertEqual(len(profile.targets), 5)
        backend = ROOT / "module/profiles" / (profile.runtime_id + ".sh")
        script = f'. "{backend}"\nprintf "%s\\n" "$NATIVE" "$HAL" "$APK" "$OFFLOAD" "$FRAMEWORK"\n'
        result = subprocess.check_output(["sh", "-c", script], text=True)
        self.assertEqual(set(result.splitlines()), {target["path"] for target in profile.targets})
        for target in profile.targets:
            self.assertIn(target["sha256"] + " " + target["path"] + "\n", profile.input_manifest())
        values = subprocess.check_output(
            [
                "sh",
                "-c",
                f'. "{backend}"\nprintf "%s\\n" "$NATIVE_PAYLOAD" "$HAL_PAYLOAD" "$APK_PAYLOAD" runtime.prop',
            ],
            text=True,
        )
        self.assertEqual(set(values.splitlines()), set(profile.data["payloads"].values()))

    def test_unknown_backend_and_profile_are_rejected(self):
        for key in ["id", "driver", "runtime_profile"]:
            with self.subTest(key=key):
                data = copy.deepcopy(self.profile.data)
                data[key] = "unverified-device"
                with self.assertRaises(ValueError):
                    self.selected(data)

    def test_paths_digests_and_duplicates_are_rejected(self):
        edits = [
            ("local_file", "../Bluetooth.apk"),
            ("path", "/vendor/../other.so"),
            ("sha256", "wrong"),
            ("path", "/vendor/has space.so"),
        ]
        for key, value in edits:
            data = copy.deepcopy(self.profile.data)
            data["targets"][0][key] = value
            with self.assertRaises(ValueError):
                self.selected(data)
        data = copy.deepcopy(self.profile.data)
        data["targets"].append(data["targets"][0])
        with self.assertRaises(ValueError):
            self.selected(data)
        data = copy.deepcopy(self.profile.data)
        data["payloads"]["hal"] = data["payloads"]["native"]
        with self.assertRaises(ValueError):
            self.selected(data)

    def test_streamed_input_hashes_and_missing_files_fail_closed(self):
        stock = self.d / "stock.so"
        stock.write_bytes(b"fixture stock")
        profile = DeviceProfile(
            self.d / "profile.json",
            {
                "id": "fixture",
                "targets": [
                    {
                        "local_file": stock.name,
                        "path": "/vendor/lib64/stock.so",
                        "sha256": digest(stock),
                    }
                ],
            },
        )
        profile.verify_inputs(self.d)
        stock.write_bytes(b"modified stock")
        with self.assertRaises(ValueError):
            profile.verify_inputs(self.d)
        stock.unlink()
        with self.assertRaises(ValueError):
            profile.verify_inputs(self.d)

    def test_generic_environment_options_and_legacy_aliases(self):
        with patch.dict(
            os.environ,
            {
                "SHIZUKA_INPUTS": str(self.d),
                "S25_BASE_BIN": "/ignored",
                "SHIZUKA_RATE": "48000",
                "S25_RATE": "96000",
                "SHIZUKA_MODE": "4096",
                "SHIZUKA_R2": "3",
            },
        ):
            self.assertEqual(inputs_dir(), self.d)
            self.assertEqual(native_options()["RATE"], 48000)
        with patch.dict(os.environ, {"SHIZUKA_RATE": "oops"}):
            with self.assertRaises(ValueError):
                native_options()

    def test_build_frontend_import_has_no_ndk_or_input_side_effects(self):
        for name in ["build.py", "build_android.py", "verify_offline.py", "build_all.py"]:
            spec = importlib.util.spec_from_file_location(
                "frontend_" + name.replace(".", "_"), ROOT / "tools" / name
            )
            module = importlib.util.module_from_spec(spec)
            with patch.dict(os.environ, {}, clear=True):
                spec.loader.exec_module(module)
            self.assertTrue(callable(module.main))

    def test_malformed_profile_shapes_are_rejected(self):
        for invalid in [None, [], {"schema": 1, "driver": []}]:
            with self.assertRaises(ValueError):
                self.selected(invalid)
        for value in [None, [], "targets"]:
            data = copy.deepcopy(self.profile.data)
            data["targets"] = value
            with self.assertRaises(ValueError):
                self.selected(data)
        data = copy.deepcopy(self.profile.data)
        data["targets"] = [None]
        with self.assertRaises(ValueError):
            self.selected(data)
        data = copy.deepcopy(self.profile.data)
        data["payloads"] = {"native": []}
        with self.assertRaises(ValueError):
            self.selected(data)

    def test_runtime_profile_selection_has_no_unverified_fallback(self):
        (self.d / "profiles").mkdir()
        for selected in ["../../outside", "unverified-device", ""]:
            (self.d / "device-profile").write_text(selected + "\n")
            script = f'MODDIR="{self.d}"\nfail() {{ echo "$*" >&2; exit 1; }}\n. "{ROOT / "module/lib/device.sh"}"\n'
            result = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)

    def test_qhs_backend_changes_exactly_the_two_reviewed_bytes(self):
        candidate = self.d / "candidate"
        candidate.write_bytes(bytes(4096))
        backend = ROOT / "module/profiles" / (self.profile.runtime_id + ".sh")
        # env dispatches dd as the host's applet; installed code always uses KernelSU BusyBox.
        script = f'. "{backend}"\nBB=env\nprofile_prepare_qhs_candidate "{candidate}"\n'
        subprocess.run(["sh", "-c", script], check=True)
        data = candidate.read_bytes()
        self.assertEqual(
            [(index, value) for index, value in enumerate(data) if value], [(3817, 3), (3953, 1)]
        )

    def test_helpers_only_build_is_independent_of_profiles_and_stock_inputs(self):
        spec = importlib.util.spec_from_file_location(
            "android_builder", ROOT / "tools/build_android.py"
        )
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)

        def dex(name):
            output = self.d / (name + "-dex/classes.dex")
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(b"synthetic dex fixture")

        with (
            patch.object(builder, "BUILD", self.d),
            patch.object(builder, "check_inputs") as inputs,
            patch.object(builder, "build_apk") as apk,
            patch.object(
                builder,
                "load_profile",
                side_effect=AssertionError("portable build read a device profile"),
            ),
            patch.object(builder, "sdk_paths", return_value=(self.d, self.d / "android.jar")),
            patch.object(builder, "build_policy", side_effect=lambda *args: dex("policy")),
            patch.object(builder, "build_status", side_effect=lambda *args: dex("status")),
            patch.object(sys, "argv", ["build_android.py", "--helpers-only"]),
        ):
            builder.main()
            inputs.assert_not_called()
            apk.assert_not_called()
        bundle = self.d / "android-tools"
        self.assertTrue((bundle / "policy.dex").is_file())
        self.assertTrue((bundle / "lib/runtime.sh").is_file())
        self.assertTrue((bundle / "lib/policy.sh").is_file())
        self.assertFalse((bundle / "device-profile").exists())
        self.assertFalse((self.d / "android-build.json").exists())
        self.assertEqual(
            (bundle / "policy.sha256").read_text().strip(), digest(bundle / "policy.dex")
        )

    def build_metadata(self):
        outputs = {
            "apk": self.profile.data["payloads"]["apk"],
            "policy": "policy-dex/classes.dex",
            "status": "status-dex/classes.dex",
        }
        for path in outputs.values():
            item = self.d / path
            item.parent.mkdir(parents=True, exist_ok=True)
            item.write_bytes(b"synthetic output")
        identity = {"profile_id": self.profile.id, "profile_fingerprint": self.profile.fingerprint}
        android = {
            "schema": 1,
            **identity,
            "sources_fingerprint": android_sources_fingerprint(),
            "apk_patches_sha256": digest(self.profile.apk_patches),
            "outputs": {name: digest(self.d / path) for name, path in outputs.items()},
        }
        (self.d / "android-build.json").write_text(json.dumps(android))
        manifest = []
        for name in self.profile.data["payloads"].values():
            path = self.d / name
            if not path.exists():
                path.write_bytes(b"synthetic payload fixture")
            manifest.append({"output": name, "output_sha256": digest(path), **identity})
        (self.d / "patch-manifest.json").write_text(json.dumps(manifest))
        record_native_build(self.profile, {"RATE": 96000, "MODE": 4096, "R2": 3}, self.d)
        native = json.loads((self.d / "native-build.json").read_text())
        return android, native

    def test_artifacts_require_current_profile_source_and_output_hashes(self):
        android, native = self.build_metadata()
        verify_android_build(self.profile, self.d)
        verify_native_build(self.profile, self.d)
        for name, original in [("android-build.json", android), ("native-build.json", native)]:
            for key in ["profile_id", "profile_fingerprint", "sources_fingerprint"]:
                changed = dict(original)
                changed[key] = "stale"
                (self.d / name).write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    (verify_android_build if name.startswith("android") else verify_native_build)(
                        self.profile, self.d
                    )
            (self.d / name).write_text(json.dumps(original))
        (self.d / "policy-dex/classes.dex").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            verify_android_build(self.profile, self.d)

    def test_native_validation_binds_artifacts_and_requires_a_real_success_boolean(self):
        self.build_metadata()
        with self.assertRaises(ValueError):
            verify_native_validation(self.profile, self.d)
        record_native_validation(self.profile, native_build_snapshot(self.profile, self.d), self.d)
        verify_native_validation(self.profile, self.d)
        path = self.d / "native-validation.json"
        original = json.loads(path.read_text())
        for field, wrong in [("passed", "true"), ("builds", {}), ("sources_fingerprint", "old")]:
            changed = {**original, field: wrong}
            path.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                verify_native_validation(self.profile, self.d)
        path.write_text(json.dumps(original))
        native = self.d / self.profile.data["payloads"]["native"]
        native.write_bytes(b"different native output")
        # A matching hand-edited manifest still cannot revive a previous build record.
        manifest = json.loads((self.d / "patch-manifest.json").read_text())
        for entry in manifest:
            if entry["output"] == native.name:
                entry["output_sha256"] = digest(native)
        (self.d / "patch-manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            verify_native_build(self.profile, self.d)
        with self.assertRaises(ValueError):
            verify_native_validation(self.profile, self.d)

    def test_malformed_manifest_and_metadata_are_reported_as_validation_errors(self):
        self.build_metadata()
        for value in [[], None, "not metadata"]:
            (self.d / "native-build.json").write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                verify_native_build(self.profile, self.d)
        android, _ = self.build_metadata()
        android["outputs"] = []
        (self.d / "android-build.json").write_text(json.dumps(android))
        with self.assertRaises(ValueError):
            verify_android_build(self.profile, self.d)
        self.build_metadata()
        for value in [None, {}, [None], [{"output": []}]]:
            (self.d / "patch-manifest.json").write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                verify_native_build(self.profile, self.d)

    def test_replacing_a_build_during_dispatch_cannot_validate_the_replacement(self):
        self.build_metadata()
        before = native_build_snapshot(self.profile, self.d)
        native = self.d / self.profile.data["payloads"]["native"]
        native.write_bytes(b"replacement build fixture")
        manifest_path = self.d / "patch-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for entry in manifest:
            if entry["output"] == native.name:
                entry["output_sha256"] = digest(native)
        manifest_path.write_text(json.dumps(manifest))
        record_native_build(self.profile, {"RATE": 48000, "MODE": 4096, "R2": 3}, self.d)
        # Both builds are individually consistent; dispatch covered the first, not its replacement.
        verify_native_build(self.profile, self.d)
        with self.assertRaisesRegex(ValueError, "Artifacts changed"):
            record_native_validation(self.profile, before, self.d)
        self.assertFalse((self.d / "native-validation.json").exists())

    def test_fingerprint_changes_with_profile_data(self):
        data = copy.deepcopy(self.profile.data)
        data["targets"][0]["sha256"] = "0" * 64
        self.assertNotEqual(self.profile.fingerprint, self.selected(data).fingerprint)

    def evidence(self):
        return [
            {
                "test": name,
                "passed": True,
                "profile_id": self.profile.id,
                "profile_fingerprint": self.profile.fingerprint,
                "sources_fingerprint": offline_sources_fingerprint(),
            }
            for name in sorted(required_tests(self.profile))
        ]

    def test_evidence_rejects_stale_profiles_duplicates_and_truthy_strings(self):
        evidence = self.evidence()
        validate_evidence(evidence, self.profile)
        for field, wrong in [
            ("passed", "false"),
            ("profile_id", "other"),
            ("profile_fingerprint", "stale"),
            ("sources_fingerprint", "old"),
        ]:
            changed = copy.deepcopy(evidence)
            changed[0][field] = wrong
            with self.assertRaises(ValueError):
                validate_evidence(changed, self.profile)
        with self.assertRaises(ValueError):
            validate_evidence(evidence + [evidence[0]], self.profile)
        with self.assertRaises(ValueError):
            validate_evidence({"passed": True}, self.profile)

    def test_package_generates_profile_and_excludes_runtime_state_and_audio_proof(self):
        # All binaries/evidence below are synthetic fixtures, deleted with the temporary directory.
        self.build_metadata()
        manifest = []
        for name in self.profile.data["payloads"].values():
            path = self.d / name
            if not path.exists():
                path.write_bytes(b"synthetic payload fixture")
            manifest.append(
                {
                    "output": name,
                    "output_sha256": digest(path),
                    "profile_id": self.profile.id,
                    "profile_fingerprint": self.profile.fingerprint,
                }
            )
        (self.d / "patch-manifest.json").write_text(json.dumps(manifest))
        record_native_build(self.profile, {"RATE": 96000, "MODE": 4096, "R2": 3}, self.d)
        repository = self.d / "repository"
        repository.mkdir()
        for directory in ["module", "config"]:
            shutil.copytree(ROOT / directory, repository / directory)
        shutil.copy2(ROOT / "LICENSE", repository / "LICENSE")
        for relative in ["src/diagnostics/dsp_capture.c", "tools/build_diagnostics.py"]:
            target = repository / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        (self.d / "dsp-capture").write_bytes(b"synthetic reader fixture")
        (self.d / "diagnostics-build.json").write_text(
            json.dumps(
                {
                    "source_sha256": digest(repository / "src/diagnostics/dsp_capture.c"),
                    "builder_sha256": digest(repository / "tools/build_diagnostics.py"),
                    "output_sha256": digest(self.d / "dsp-capture"),
                }
            )
        )
        for name in [
            "state/active",
            "proofs/verified-payloads.sha256",
            "payload/untracked.so",
            ".no-auto",
            ".enable-dual-mode",
            "disable",
            "latency-provider",
            "latency-provider.sha256",
            "latency-runtime-build.json",
            "latency-runtime.identity",
        ]:
            path = repository / "module" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("previous device state must not be shipped")
        (repository / "evidence").mkdir()
        (repository / "evidence/ll-offline-tests.json").write_text(json.dumps(self.evidence()))
        spec = importlib.util.spec_from_file_location("package_fixture", ROOT / "tools/package.py")
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        package.ROOT, package.BUILD, package.DIST = repository, self.d, self.d / "dist"
        (self.d / "Shizuka-settings.apk").write_bytes(b"synthetic companion APK fixture")
        package.settings_sources_fingerprint = lambda: "synthetic-settings-sources"
        (self.d / "settings-app-build.json").write_text(json.dumps({
            "schema": 1, "package": package.PACKAGE, "version_code": package.VERSION_CODE,
            "sources_sha256": "synthetic-settings-sources",
            "apk_sha256": digest(self.d / "Shizuka-settings.apk"),
        }))
        with self.assertRaises(ValueError):
            package.main()
        self.assertFalse(package.DIST.exists())
        record_native_validation(self.profile, native_build_snapshot(self.profile, self.d), self.d)
        with contextlib.redirect_stdout(io.StringIO()):
            package.main()
        with zipfile.ZipFile(next(package.DIST.glob("*.zip"))) as archive:
            names = set(archive.namelist())
            self.assertTrue(
                {
                    "LICENSE",
                    "lib/device.sh",
                    "lib/config.sh",
                    "lib/runtime.sh",
                    "policy.dex",
                    "status.dex",
                    "proofs/offline-ready",
                    "settings.apk",
                    "settings.sha256",
                    "settings-bridge.sh",
                }.issubset(names)
            )
            self.assertEqual(
                archive.read("device-profile").decode().strip(), self.profile.runtime_id
            )
            self.assertEqual(archive.read("inputs.sha256").decode(), self.profile.input_manifest())
            self.assertFalse(any(name.startswith("state/") for name in names))
            self.assertFalse(
                {
                    "proofs/verified-payloads.sha256",
                    ".no-auto",
                    ".enable-dual-mode",
                    "disable",
                    "payload/untracked.so",
                    "latency-provider",
                    "latency-provider.sha256",
                    "latency-runtime-build.json",
                    "latency-runtime.identity",
                }.intersection(names)
            )
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.read("settings.apk"), b"synthetic companion APK fixture")
            self.assertEqual(
                {name for name in names if name.startswith("webroot/")},
                set(package.NATIVE_ENTRY_ASSETS),
            )
            for name, expected in package.NATIVE_ENTRY_ASSETS.items():
                self.assertEqual(package.sha(archive.read(name)), expected)
            self.assertEqual(
                {
                    name for name in names
                    if pathlib.PurePosixPath(name).suffix.lower() in {".html", ".htm", ".css", ".js"}
                },
                set(package.NATIVE_ENTRY_ASSETS),
            )
        # Optional extras have their own identity and never join the core
        # manifest or inherit a heard-audio proof from the source module.
        latency = {
            "schema": 1, "activation_requested": False, "device_proof": False,
            "diagnostic_only": False, "diagnostic_zero_ttp": False,
            "experimental_raw_island": False,
            "cookie": "abcd" * 8, "outputs": {},
            "core": {"outputs": {
                role: digest(self.d / self.profile.data["payloads"][role])
                for role in ("native", "hal")
            }},
        }
        for name in ("latency-provider", "patched-libar-gsl.so"):
            (self.d / name).write_bytes(("verified synthetic " + name).encode())
            latency["outputs"][name] = digest(self.d / name)
        (self.d / package.LATENCY_BUILD).write_text(json.dumps(latency))
        with patch.object(package, "verify_latency_runtime", return_value=latency), contextlib.redirect_stdout(io.StringIO()):
            package.main(with_latency_runtime=True)
        with zipfile.ZipFile(next(package.DIST.glob("*-latency.zip"))) as archive:
            self.assertEqual(archive.read("latency-provider"), (self.d / "latency-provider").read_bytes())
            self.assertEqual(archive.read("payload/patched-libar-gsl.so"), (self.d / "patched-libar-gsl.so").read_bytes())
            self.assertEqual(
                archive.read("latency-runtime.identity").decode(),
                package.latency_runtime_identity(latency),
            )
            self.assertEqual(len(archive.read("payloads.sha256").splitlines()), 4)
            self.assertEqual(archive.read("proofs/offline-ready").decode(), package.sha(archive.read("payloads.sha256")) + "\n")
            self.assertNotIn("proofs/verified-payloads.sha256", archive.namelist())
            self.assertFalse(any(name.startswith("state/") for name in archive.namelist()))
        with patch.object(package, "verify_latency_runtime", side_effect=ValueError("stale latency identity")):
            with self.assertRaisesRegex(ValueError, "stale latency identity"):
                package.main(with_latency_runtime=True)
        # An obsolete UI copied into an upgrade checkout must not be shipped.
        for relative in [
            "webroot/status.js", "webroot/status.css", "webroot/extra.html",
            "webroot/nested/entry", "webui/status.js", "status.css",
        ]:
            path = repository / "module" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("obsolete browser UI fixture")
            with self.assertRaisesRegex(ValueError, "Only native settings UI is supported"):
                package.main()
            path.unlink()
            if path.parent not in {repository / "module", repository / "module/webroot"}:
                path.parent.rmdir()
        for relative in package.NATIVE_ENTRY_ASSETS:
            path = repository / "module" / relative
            original = path.read_bytes()
            path.write_bytes(original + b"\nmodified launcher fixture\n")
            with self.assertRaisesRegex(ValueError, "unapproved launcher asset"):
                package.main()
            path.write_bytes(original)
            path.unlink()
            with self.assertRaisesRegex(ValueError, "native launcher is incomplete"):
                package.main()
            path.write_bytes(original)
        target = repository / "module/webroot/launcher.js"
        original = target.read_bytes()
        target.unlink()
        target.symlink_to(ROOT / "module/webroot/launcher.js")
        with self.assertRaisesRegex(ValueError, "unapproved launcher asset"):
            package.main()
        target.unlink()
        target.write_bytes(original)
        (self.d / "Shizuka-settings.apk").write_bytes(b"changed after build")
        with self.assertRaisesRegex(ValueError, "Settings APK is stale"):
            package.main()

    def test_kernelsu_native_entry_has_only_a_fixed_one_shot_launch(self):
        node = shutil.which("node")
        self.assertIsNotNone(node, "Node.js is required to verify the KernelSU entry bridge")
        result = subprocess.run(
            [node, str(ROOT / "tests/native_settings_entry_test.cjs")],
            capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
