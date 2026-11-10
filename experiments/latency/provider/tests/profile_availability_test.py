#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual profile's verified native-provider availability boundary."""
import hashlib
import json
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
PROFILE = ROOT / "module/profiles/samsung-sm-s931z-dzif.sh"


class Availability(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.module = Path(self.temp.name)
        self.binary = self.module / "latency-provider"
        self.digest = self.module / "latency-provider.sha256"
        self.busybox = self.module / "busybox"
        self.busybox.write_text(
            "#!/usr/bin/env python3\nimport hashlib,os,sys\n"
            "if sys.argv[1]=='sh':os.execvp('sh',['sh',*sys.argv[2:]])\n"
            "assert sys.argv[1]=='sha256sum'\n"
            "print(hashlib.sha256(open(sys.argv[2],'rb').read()).hexdigest()+'  '+sys.argv[2])\n")
        self.busybox.chmod(0o755)

    def run_profile(self):
        script = (f"MODDIR={shlex.quote(str(self.module))}\nSTATE=\"$MODDIR/state\"\nBB={shlex.quote(str(self.busybox))}\n"
                  f". {shlex.quote(str(PROFILE))}\nprofile_tuning_availability\n")
        result = subprocess.run(["sh", "-eu", "-c", script], text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def provider(self):
        self.binary.write_text(
            "#!/bin/sh\n"
            '[ "$1" = --status ] && [ "$2" = --module-dir ] && [ "$3" = "$(dirname "$0")" ] || exit 17\n'
            "printf '%s\\n' '{\"available\":false,\"applied\":false,\"status\":\"unavailable\",\"reason\":\"No current runtime observation\"}'\n")
        self.binary.chmod(0o755)
        self.digest.write_text(hashlib.sha256(self.binary.read_bytes()).hexdigest() + "\n")

    def test_missing_provider_retains_explicit_unavailable_backend(self):
        status = self.run_profile()
        self.assertFalse(status["runtime_apply_supported"])
        self.assertEqual(status["status"], "runtime_not_packaged")
        self.assertIn("Signed DSP firmware is unchanged", status["reason"])

    def test_verified_provider_receives_only_readonly_status_arguments(self):
        self.provider()
        status = self.run_profile()
        self.assertFalse(status["available"])
        self.assertEqual(status["reason"], "No current runtime observation")

    def test_changed_binary_cannot_be_executed_as_provider(self):
        self.provider()
        self.digest.write_text("0" * 64 + "\n")
        self.assertEqual(self.run_profile()["status"], "runtime_not_packaged")

    def test_owned_recovery_fence_overrides_any_provider_observation(self):
        self.provider()
        state = self.module / "state"
        state.mkdir()
        (state / "latency-remote-uncertain").write_text("private recovery fence fixture\n")
        (self.module / "latency-runtime.sh").write_text(
            '#!/bin/sh\n[ "$1" = recovery-status ] || exit 91\n'
            "printf '%s\\n' '{\"owned_recovery_required\":true,\"available\":false,\"applied\":false}'\n"
        )
        result = self.run_profile()
        self.assertTrue(result["owned_recovery_required"])
        self.assertFalse(result["available"])
        self.assertFalse(result["applied"])

    def test_symlink_binary_and_hash_are_not_followed(self):
        self.provider()
        target = self.module / "other-provider"
        self.binary.rename(target)
        self.binary.symlink_to(target)
        self.assertEqual(self.run_profile()["status"], "runtime_not_packaged")
        self.binary.unlink()
        target.rename(self.binary)
        hash_target = self.module / "other-hash"
        self.digest.rename(hash_target)
        self.digest.symlink_to(hash_target)
        self.assertEqual(self.run_profile()["status"], "runtime_not_packaged")


if __name__ == "__main__":
    unittest.main()
