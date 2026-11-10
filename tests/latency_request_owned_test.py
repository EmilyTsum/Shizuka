# SPDX-License-Identifier: GPL-2.0-only
"""The real runtime request body reuses authenticated inherited lifecycle ownership."""
import importlib.util
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("owned_request_fixture", ROOT / "tests/latency_request_test.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)
ARGS = ["2", "ed22fba8-41da-4c3b-baf7-15350e1acc7e", "150", "0", "200000", "a"*64, "b"*64, "0", "123", "456"]


class OwnedRequest(unittest.TestCase):
    def setUp(self):
        previous_umask = os.umask(0o022)
        self.addCleanup(os.umask, previous_umask)
        self.r = FIXTURE.LatencyRequest(methodName="runTest")
        self.addCleanup(self.r.doCleanups)
        self.r.setUp()
        self.f, self.d, self.state = self.r.f, self.r.d, self.r.state
        runtime = self.d / "lib/runtime.sh"
        runtime.write_text(runtime.read_text() + '\nlock_lifecycle() { echo relock >> "$MODDIR/relocks";exit 91; }\n')
        guard = self.d / "latency-apply-owned.sh"
        guard.write_text('''#!/bin/sh
printf '%s\n' "$*" >> "${0%/*}/guard-calls"
[ "$#" = 11 ] && [ "$1" = validate-request ] && [ -e /dev/fd/9 ] || exit 20
exit "${TEST_GUARD_RESULT:-0}"
''')
        guard.chmod(0o644)

    def call(self, *, guard_result="0", args=None):
        env = dict(self.f.env, TEST_GUARD_RESULT=guard_result)
        command = 'exec 9> "$TEST_ROOT/state/lifecycle.lock"\nexec sh "$TEST_ROOT/latency-runtime.sh" request-owned "$@"'
        return subprocess.run(["sh", "-c", command, "owned-test", *(ARGS if args is None else args)],
                              env=env, capture_output=True, text=True, timeout=6)

    def test_same_body_creates_only_typed_request_without_relocking(self):
        result = self.call()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.r.request.read_bytes(), self.r.expected_request())
        self.assertFalse((self.d / "relocks").exists())
        self.assertEqual((self.d / "guard-calls").read_text().strip(), "validate-request " + " ".join(ARGS))
        self.r.assert_no_activation()

    def test_unqualified_or_rejected_guard_cannot_prepare_or_replace(self):
        for code in ("10", "20"):
            with self.subTest(code=code):
                result = self.call(guard_result=code)
                self.assertEqual(result.returncode, int(code))
                self.assertFalse(self.r.request.exists())
                self.assertFalse((self.d / "relocks").exists())
                self.r.assert_no_activation()

    def test_payload_validation_still_runs_after_guard(self):
        self.r.native.write_bytes(b"changed native payload")
        result = self.call()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.r.request.exists())
        self.r.assert_no_activation()

    def test_missing_or_symlink_guard_cannot_prepare(self):
        guard = self.d / "latency-apply-owned.sh"
        original = guard.read_bytes()
        guard.unlink()
        self.assertNotEqual(self.call().returncode, 0)
        (self.d / "outside-guard").write_bytes(original)
        guard.symlink_to(self.d / "outside-guard")
        self.assertNotEqual(self.call().returncode, 0)
        self.assertFalse(self.r.request.exists())

    def test_exact_owned_argv_is_required(self):
        self.assertNotEqual(self.call(args=ARGS[:-1]).returncode, 0)
        self.assertNotEqual(self.call(args=ARGS+["extra"]).returncode, 0)
        self.assertFalse((self.d / "guard-calls").exists())
        self.assertFalse(self.r.request.exists())


if __name__ == "__main__":
    unittest.main()
