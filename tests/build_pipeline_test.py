# SPDX-License-Identifier: GPL-2.0-only
"""Build orchestration failures must never publish or revive verification evidence."""

import contextlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from shizuka.artifacts import NATIVE_BUILD, NATIVE_VALIDATION, write_metadata  # noqa: E402
from shizuka.profiles import load_profile  # noqa: E402


def frontend(name):
    spec = importlib.util.spec_from_file_location(
        "pipeline_" + name, ROOT / "tools" / (name + ".py")
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Pipeline(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.root / "evidence").mkdir()
        self.host = self.root / "evidence/ll-offline-tests.json"
        self.native = self.build / NATIVE_VALIDATION
        self.profile = load_profile("samsung-sm-s931z-dzif")

    @contextlib.contextmanager
    def verification(self, *, host_only=False):
        module = frontend("verify_offline")
        events = []

        def rebuild(*args, **kwargs):
            self.assertFalse(self.host.exists())
            events.append("build")

        def dispatch(profile):
            self.assertIs(profile, self.profile)
            events.append("dispatch")

        def record(profile, builds):
            self.assertEqual(events, ["build", "dispatch"])
            self.assertEqual(builds, {"native-build.json": "captured-before-dispatch"})
            events.append("record")
            self.native.write_text("synthetic integration fixture")

        backend = SimpleNamespace(verify_dispatch=Mock(side_effect=dispatch))
        with contextlib.ExitStack() as stack:
            for name, value in {
                "ROOT": self.root,
                "BUILD": self.build,
                "PYTHON_CASES": (),
            }.items():
                stack.enter_context(patch.object(module, name, value))
            mocks = {}
            for name, options in {
                "load_profile": {"return_value": self.profile},
                "verify_native": {"return_value": [{"test": "fixture", "passed": True}]},
                "verify_policies": {"return_value": []},
                "validate_evidence": {},
                "offline_sources_fingerprint": {"return_value": "unchanged-sources"},
                "get_backend": {"return_value": backend},
                "native_build_snapshot": {
                    "return_value": {"native-build.json": "captured-before-dispatch"}
                },
                "record_native_validation": {"side_effect": record},
            }.items():
                mocks[name] = stack.enter_context(patch.object(module, name, **options))
            mocks["rebuild"] = stack.enter_context(
                patch.object(module.subprocess, "run", side_effect=rebuild)
            )
            arguments = ["verify_offline.py", *(["--host-only"] if host_only else [])]
            stack.enter_context(patch.object(sys, "argv", arguments))
            yield module, events, mocks, backend

    def test_complete_verification_records_only_after_dispatch_passes(self):
        self.host.write_text("old host evidence")
        self.native.write_text("old integration evidence")
        with self.verification() as (module, events, mocks, backend):
            module.main()
        self.assertEqual(events, ["build", "dispatch", "record"])
        self.assertEqual(json.loads(self.host.read_text())[0]["passed"], True)
        self.assertEqual(self.native.read_text(), "synthetic integration fixture")

    def test_host_only_never_claims_native_integration(self):
        with self.verification(host_only=True) as (module, events, mocks, backend):
            module.main()
            mocks["rebuild"].assert_not_called()
            mocks["native_build_snapshot"].assert_not_called()
            backend.verify_dispatch.assert_not_called()
            mocks["record_native_validation"].assert_not_called()
        self.assertTrue(self.host.is_file())
        self.assertFalse(self.native.exists())
        self.assertEqual(events, [])

    def test_failed_host_build_or_dispatch_cannot_leave_old_success_evidence(self):
        for failure in ("host", "build", "dispatch"):
            with self.subTest(failure=failure):
                self.host.write_text("old host evidence")
                self.native.write_text("old integration evidence")
                with self.verification() as (module, events, mocks, backend):
                    failing = {
                        "host": mocks["verify_native"],
                        "build": mocks["rebuild"],
                        "dispatch": backend.verify_dispatch,
                    }[failure]
                    failing.side_effect = RuntimeError("expected failure")
                    with self.assertRaisesRegex(RuntimeError, "expected failure"):
                        module.main()
                    mocks["record_native_validation"].assert_not_called()
                self.assertFalse(self.host.exists())
                self.assertFalse(self.native.exists())

    def test_source_change_during_tests_does_not_publish_evidence(self):
        for observed in (["before", "after"], ["before", "before", "after"]):
            with self.subTest(observed=observed):
                with self.verification() as (module, events, mocks, backend):
                    mocks["offline_sources_fingerprint"].side_effect = observed
                    with self.assertRaisesRegex(ValueError, "Sources changed"):
                        module.main()
                    mocks["record_native_validation"].assert_not_called()
                self.assertFalse(self.host.exists())
                self.assertFalse(self.native.exists())

    def test_rebuild_invalidates_metadata_before_input_checks(self):
        module = frontend("build")
        for name in (NATIVE_BUILD, NATIVE_VALIDATION):
            (self.build / name).write_text("old build record")
        profile = SimpleNamespace(verify_inputs=Mock(side_effect=ValueError("stock mismatch")))
        with (
            patch.object(module, "BUILD", self.build),
            patch.object(module, "load_profile", return_value=profile),
        ):
            with self.assertRaisesRegex(ValueError, "stock mismatch"):
                module.main()
        for name in (NATIVE_BUILD, NATIVE_VALIDATION):
            self.assertFalse((self.build / name).exists())

    def test_android_rebuild_invalidates_metadata_before_input_checks(self):
        module = frontend("build_android")
        for name in ("android-build.json", NATIVE_VALIDATION):
            (self.build / name).write_text("old build record")
        with (
            patch.object(module, "BUILD", self.build),
            patch.object(module, "check_inputs", side_effect=ValueError("stock mismatch")),
            patch.object(sys, "argv", ["build_android.py"]),
        ):
            with self.assertRaisesRegex(ValueError, "stock mismatch"):
                module.main()
        for name in ("android-build.json", NATIVE_VALIDATION):
            self.assertFalse((self.build / name).exists())

    def test_failed_metadata_write_never_replaces_a_complete_record(self):
        path = self.build / "record.json"
        path.write_text('{"previous": true}\n')
        with patch("shizuka.artifacts.json.dump", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(OSError, "disk full"):
                write_metadata(path, {"passed": True})
        self.assertEqual(json.loads(path.read_text()), {"previous": True})
        self.assertEqual(list(self.build.iterdir()), [path])

    def test_build_all_stops_before_packaging_after_a_failed_stage(self):
        stages = ["build_android.py", "build_settings_app.py", "verify_offline.py", "package.py"]
        for index, stage in enumerate(stages):
            with self.subTest(failed_stage=stage), patch("subprocess.run") as run:
                module = frontend("build_all")
                run.assert_not_called()
                run.side_effect = [None] * index + [subprocess.CalledProcessError(1, stage)]
                with self.assertRaises(subprocess.CalledProcessError):
                    module.main()
                self.assertEqual(
                    [pathlib.Path(call.args[0][1]).name for call in run.call_args_list],
                    stages[:index + 1],
                )


if __name__ == "__main__":
    unittest.main()
