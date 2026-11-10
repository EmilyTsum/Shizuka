#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The actual fixed handler's source gate cannot be bypassed by mode or environment."""
import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
ARGS = ["2", "ed22fba8-41da-4c3b-baf7-15350e1acc7e", "150", "0", "200000", "a"*64, "b"*64, "0", "123", "456"]


class OwnedGate(unittest.TestCase):
    def setUp(self):
        previous_umask = os.umask(0o022)
        self.addCleanup(os.umask, previous_umask)
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-owned-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.d = Path(self.temporary.name).resolve()
        (self.d / "lib").mkdir()
        (self.d / "state").mkdir(mode=0o700)
        (self.d / "lib/runtime.sh").write_text("ensure_busybox_shell() { :; }\n")
        source = (ROOT / "module/latency-apply-owned.sh").read_text()
        source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        source = source.replace('[ "$(id -u)" = 0 ]', '[ 0 = 0 ]')
        self.entry = self.d / "latency-apply-owned.sh"
        self.entry.write_text(source)
        bb = self.d / "bb"
        bb.write_text(f"#!{sys.executable}\n" + r'''
import os,pathlib,stat,sys
a=sys.argv[1:]
if a[0]=='readlink':print(pathlib.Path(a[-1]).resolve())
elif a[0]=='stat':
 s=pathlib.Path(a[-1]).stat();print('0:'+format(stat.S_IMODE(s.st_mode),'o')+':'+str(s.st_nlink))
elif a[0]=='sh':os.execv('/bin/sh',['sh',*a[1:]])
else:
 pathlib.Path(os.environ['FIXTURE_ROOT'],'unexpected').write_text(str(a));raise SystemExit(91)
''')
        bb.chmod(0o700)
        self.env = {**os.environ, "FIXTURE_ROOT": str(self.d), "QUALIFIED": "1", "SHIZUKA_LATENCY_QUALIFIED": "1"}

    def test_execute_and_both_guard_modes_remain_unqualified(self):
        for mode in ("execute", "validate-request", "validate-health"):
            with self.subTest(mode=mode):
                result = subprocess.run(["sh", str(self.entry), mode, *ARGS], env=self.env,
                                        capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 10, result.stderr)
                self.assertEqual(list((self.d / "state").iterdir()), [])
                self.assertFalse((self.d / "unexpected").exists())

    def test_wrong_mode_or_argument_count_rejects(self):
        for args in (["anything", *ARGS], ["execute", *ARGS[:-1]], ["execute", *ARGS, "extra"]):
            result = subprocess.run(["sh", str(self.entry), *args], env=self.env,
                                    capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list((self.d / "state").iterdir()), [])

    def executor_fixture(self, *, simulated_qualification=False):
        source = (ROOT / "module/latency-apply-executor.sh").read_text()
        if simulated_qualification:
            # Only this disposable host fixture changes its gate. The checked-in
            # production source remains false, and no Android command is used.
            self.assertEqual(source.count("QUALIFIED=0"), 1)
            source = source.replace("QUALIFIED=0", "QUALIFIED=1")
        source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        source = source.replace('[ "$(id -u)" = 0 ]', '[ 0 = 0 ]')
        path = self.d / "latency-apply-executor.sh"
        path.write_text(source)
        (self.d / "latency-apply-owned.sh").write_text("# Fixed-owner placeholder; never executed by the host spy.\n")
        (self.d / "control.sh").write_text('exec ' + sys.executable + ' "${0%/*}/control-spy.py" "$@"\n')
        (self.d / "control-spy.py").write_text('''import json,os,pathlib,sys
try:os.fstat(9);inherited=True
except OSError:inherited=False
pathlib.Path(os.environ['FIXTURE_ROOT'],'forwarded').write_text(json.dumps({'args':sys.argv[1:],'fd9':inherited}))
raise SystemExit(int(os.environ.get('FIXTURE_CONTROL_EXIT','0')))
''')
        return path

    def test_executor_gate_cannot_be_enabled_by_arguments_or_environment(self):
        entry = self.executor_fixture()
        for args in ([], [*ARGS, "1"], ["anything", "$(touch injected)"]):
            result = subprocess.run(["sh", str(entry), *args], env=self.env, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 10)
        self.assertFalse((self.d / "forwarded").exists())
        self.assertFalse((self.d / "unexpected").exists())

    def test_fixed_executor_forwarder_preserves_lock_and_drops_local_sequence(self):
        entry = self.executor_fixture(simulated_qualification=True)
        lock = self.d / "state/lifecycle.lock"
        lock.touch()
        result = subprocess.run(["sh", "-c", 'exec 9<"$1";shift;exec sh "$@"', "fixture", str(lock), str(entry), *ARGS, "7"],
                                env=dict(self.env, FIXTURE_CONTROL_EXIT="30"), capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 30, result.stderr)
        actual = json.loads((self.d / "forwarded").read_text())
        self.assertEqual(actual, {"args": ["latency-owned", *ARGS], "fd9": True})

    def test_fixed_executor_rejects_bad_channel_sequence_before_control(self):
        entry = self.executor_fixture(simulated_qualification=True)
        for sequence in ("0", "01", "10001", "1;exit 0", ""):
            result = subprocess.run(["sh", str(entry), *ARGS, sequence], env=self.env, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 20)
        self.assertFalse((self.d / "forwarded").exists())


if __name__ == "__main__":
    unittest.main()
