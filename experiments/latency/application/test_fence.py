#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual fixed fence dispatcher with real flock and private fixtures."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class Fence(unittest.TestCase):
    def setUp(self):
        previous_umask = os.umask(0o022)
        self.addCleanup(os.umask, previous_umask)
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-fence-")
        self.addCleanup(self.temporary.cleanup)
        self.d = Path(self.temporary.name).resolve()
        (self.d / "state").mkdir(mode=0o700)
        source = (ROOT / "module/latency-apply-fence.sh").read_text()
        source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        source = source.replace('[ "$(id -u)" = 0 ]', '[ 0 = 0 ]')
        source = source.replace("/system/bin/app_process", '"$MODDIR/app_process"')
        self.entry = self.d / "latency-apply-fence.sh"
        self.entry.write_text(source)
        bb = self.d / "bb"
        bb.write_text(f"#!{sys.executable}\n" + r'''
import fcntl,hashlib,os,pathlib,stat,sys
a=sys.argv[1:]
if a[0]=='readlink':print(pathlib.Path(a[-1]).resolve())
elif a[0]=='stat':
 p=a[-1];s=os.fstat(9) if p=='/proc/self/fd/9' else os.stat(p) if a[1]=='-Lc' else os.lstat(p);f=a[2]
 if f=='%u:%a':print('0:'+format(stat.S_IMODE(s.st_mode),'o'))
 elif f=='%u:%a:%h':print('0:'+format(stat.S_IMODE(s.st_mode),'o')+':'+str(s.st_nlink))
 elif f=='%d:%i':print(str(s.st_dev)+':'+str(s.st_ino))
 elif f=='%s':print(s.st_size)
 else:raise SystemExit(90)
elif a[0]=='sha256sum':print(hashlib.sha256(pathlib.Path(a[1]).read_bytes()).hexdigest()+'  '+a[1])
elif a[0]=='find':
 for p in pathlib.Path(a[1]).iterdir():print(p);break
elif a[0]=='flock':
 try:fcntl.flock(int(a[-1]),fcntl.LOCK_EX|fcntl.LOCK_NB)
 except BlockingIOError:raise SystemExit(1)
else:raise SystemExit(91)
''')
        bb.chmod(0o700)
        app = self.d / "app_process"
        app.write_text(f"#!{sys.executable}\n" + r'''
import json,os,pathlib,sys
d=pathlib.Path(os.environ['FIXTURE_ROOT']);(d/'calls').write_text(json.dumps(sys.argv[1:]));print('{"fixture":true}')
''')
        app.chmod(0o700)
        self.env = {**os.environ, "FIXTURE_ROOT": str(self.d)}

    def run_entry(self, mode, fd=None):
        if fd is None:
            return subprocess.run(["sh", str(self.entry), mode], env=self.env,
                                  text=True, capture_output=True, timeout=3)
        return subprocess.run(["sh", "-c", 'exec 9<&"$1";shift;exec sh "$@"', "fixture", str(fd), str(self.entry), mode],
                              env=self.env, text=True, capture_output=True, timeout=3, pass_fds=(fd,))

    def journal(self):
        d = self.d / "state/latency-application"
        d.mkdir(mode=0o700)
        return d

    def dex(self):
        value = b"synthetic fixed dispatcher DEX"
        (self.d / "latency-application.dex").write_bytes(value)
        (self.d / "latency-application.sha256").write_text(hashlib.sha256(value).hexdigest()+"\n")

    def test_no_journal_or_empty_protected_journal_is_clear_without_dex(self):
        self.assertEqual(self.run_entry("check").returncode, 0)
        self.journal()
        self.assertTrue(json.loads(self.run_entry("health").stdout)["healthy"])

    def test_unknown_journal_with_missing_dex_is_never_cleared(self):
        d = self.journal()
        for name in ("current", "unknown", "owned.invalid"):
            p = d / name
            p.write_text("not proof")
            result = self.run_entry("check")
            self.assertEqual(result.returncode, 3)
            self.assertTrue(p.exists())
            p.unlink()

    def test_symlink_or_nonprivate_journal_rejects(self):
        d = self.journal()
        d.chmod(0o755)
        self.assertEqual(self.run_entry("check").returncode, 3)
        d.rmdir()
        d.symlink_to(self.d / "elsewhere")
        self.assertEqual(self.run_entry("check").returncode, 3)

    def test_fixed_readonly_dispatch_and_dex_digest_binding(self):
        self.journal()
        self.dex()
        self.assertEqual(self.run_entry("check").returncode, 0)
        args = json.loads((self.d / "calls").read_text())
        self.assertEqual(args, ["/system/bin", "LatencyApplyFenceMain", "--module-dir", str(self.d), "check"])
        (self.d / "calls").unlink()
        (self.d / "latency-application.dex").write_bytes(b"changed")
        self.assertEqual(self.run_entry("check").returncode, 3)
        self.assertFalse((self.d / "calls").exists())

    def test_reconcile_requires_exact_inherited_lifecycle_descriptor(self):
        self.journal()
        self.dex()
        self.assertEqual(self.run_entry("reconcile").returncode, 3)
        with (self.d / "state/wrong.lock").open("w") as wrong:
            self.assertEqual(self.run_entry("reconcile", wrong.fileno()).returncode, 3)
        with (self.d / "state/lifecycle.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            result = self.run_entry("reconcile", lock.fileno())
            self.assertEqual(result.returncode, 0, result.stderr)
            args = json.loads((self.d / "calls").read_text())
            self.assertEqual(args, ["/system/bin", "LatencyApplyReconcileMain", "--module-dir", str(self.d)])

    def test_invalid_mode_cannot_dispatch(self):
        self.journal()
        self.dex()
        self.assertEqual(self.run_entry("reconcile;play").returncode, 3)
        self.assertFalse((self.d / "calls").exists())


if __name__ == "__main__":
    unittest.main()
