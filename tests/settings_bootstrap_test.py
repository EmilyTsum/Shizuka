# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the fixed normal-app root entry using temporary Android boundaries."""

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))
SOCKET = "shizuka-settings-01234567-89ab-cdef-0123-456789abcdef"


class SettingsBootstrap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="shizuka-settings-bootstrap-")
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name).resolve()
        self.module = self.d / "module"
        self.module.mkdir()
        self.calls = self.d / "calls.jsonl"
        self.env = dict(
            os.environ,
            PATH=str(self.d) + os.pathsep + os.environ["PATH"],
            FIX_MODULE=str(self.module),
            FIX_CALLS=str(self.calls),
            FIX_SOCKET=SOCKET,
            FIX_INSTALLED=str(self.d / "installed.apk"),
        )
        source = (ROOT / "module/settings-bootstrap.sh").read_text()
        self.script = self.d / "bootstrap.sh"
        self.script.write_text(
            source.replace(
                "MODDIR=/data/adb/modules/s25_aptx_adaptive",
                "MODDIR=" + shlex.quote(str(self.module)),
            ).replace("BB=/data/adb/ksu/bin/busybox", "BB=" + shlex.quote(str(self.d / "bb")))
        )
        self.bin("id", f"#!{sys.executable}\nimport os\nprint(os.environ.get('FIX_UID','0'))\n")
        self.bin(
            "bb",
            f"#!{sys.executable}\n"
            + """import hashlib, os, pathlib, stat, sys
a=sys.argv[1:]
if a[:3]==['stat','-c','%u:%a:%h']:
 p=pathlib.Path(a[3]);s=p.stat();owner='1000' if a[3]==os.environ.get('FIX_UNSAFE_OWNER') else '0'
 print(owner+':'+format(stat.S_IMODE(s.st_mode),'o')+':'+str(s.st_nlink))
elif a[:2]==['readlink','-f']:print(os.path.realpath(a[2]))
elif a[0]=='sha256sum':print(hashlib.sha256(pathlib.Path(a[1]).read_bytes()).hexdigest()+'  '+a[1])
else:os.execvp(a[0],a)
""",
        )
        self.bin("am", f"#!{sys.executable}\nimport os\nprint(os.environ.get('FIX_USER','0'))\n")
        self.bin(
            "cmd",
            f"#!{sys.executable}\n"
            + """import json,os,sys
with open(os.environ['FIX_CALLS'],'a') as out:out.write(json.dumps(['cmd',*sys.argv[1:]])+'\\n')
assert sys.argv[1:]==['package','list','packages','-U','--user',os.environ.get('FIX_USER','0'),'dev.shizuka.settings']
print('package:dev.shizuka.settings uid:'+os.environ.get('FIX_PACKAGE_UID','10123'))
""",
        )
        self.bin(
            "pm",
            f"#!{sys.executable}\n"
            + """import json,os,sys
with open(os.environ['FIX_CALLS'],'a') as out:out.write(json.dumps(['pm',*sys.argv[1:]])+'\\n')
assert sys.argv[1:]==['path','--user',os.environ.get('FIX_USER','0'),'dev.shizuka.settings']
print('package:'+os.environ['FIX_INSTALLED'])
""",
        )
        for file, content in {
            "module.prop": "id=s25_aptx_adaptive\n",
            "settings.apk": "same-apk-bytes",
            "settings.sha256": hashlib.sha256(b"same-apk-bytes").hexdigest() + "\n",
            "ui-lifecycle.sh": 'printf "ui-owner %s\\n" "$*" >> "$FIX_MODULE/entry.calls"\n',
            "settings-bridge.sh": 'printf "bridge %s\\n" "$*" >> "$FIX_MODULE/entry.calls"\nprintf "%s\\n" "$FIX_SOCKET"\n',
        }.items():
            path = self.module / file
            path.write_text(content)
            path.chmod(0o644)
        (self.d / "installed.apk").write_bytes(b"same-apk-bytes")

    def bin(self, name, text):
        p = self.d / name
        p.write_text(text)
        p.chmod(0o755)

    def run_entry(self, *args, ok=True, env=None):
        r = subprocess.run(
            [*SHELL, str(self.script), *args],
            env=self.env if env is None else env,
            capture_output=True,
            text=True,
            timeout=5,
        )
        if ok:
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout)
        value = json.loads(r.stdout)
        self.assertEqual(value["scope"], "settings_session")
        self.assertIs(value["available"], ok)
        return value

    def no_bridge(self):
        self.assertFalse((self.module / "entry.calls").exists())

    def test_only_validated_ui_bridge_is_started(self):
        r = self.run_entry("10123")
        self.assertEqual(r["socket"], SOCKET)
        self.assertEqual(r["companion_uid"], 10123)
        self.assertEqual(
            (self.module / "entry.calls").read_text().splitlines(),
            ["ui-owner start", "bridge start 10123"],
        )
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertEqual(
            calls[-1][1], "path"
        )  # No installation, activation or configuration command.

    def test_exact_current_user_uid_binding(self):
        self.run_entry("110123", env=dict(self.env, FIX_USER="1", FIX_PACKAGE_UID="110123"))
        self.assertIn("bridge start 110123", (self.module / "entry.calls").read_text())

    def test_wrong_uid_or_nonroot_cannot_start_bridge(self):
        self.run_entry("10124", ok=False)
        self.run_entry("10123", ok=False, env=dict(self.env, FIX_UID="2000"))
        self.no_bridge()

    def test_invalid_arguments_are_rejected_before_android_calls(self):
        for args in (
            (),
            ("10123", "extra"),
            *[
                (x,)
                for x in (
                    "",
                    "010123",
                    "0",
                    "9999",
                    "-1",
                    "+10123",
                    "10123\n",
                    "2147483648",
                    "999999999999999999999",
                    "$(id)",
                )
            ],
        ):
            with self.subTest(args=args):
                self.run_entry(*args, ok=False)
        self.assertFalse(self.calls.exists())
        self.no_bridge()

    def test_disabled_removed_or_symlink_markers_refuse(self):
        for name in ("disable", "remove"):
            p = self.module / name
            p.touch()
            self.run_entry("10123", ok=False)
            p.unlink()
            p.symlink_to(self.module / "missing")
            self.run_entry("10123", ok=False)
            p.unlink()
        self.no_bridge()

    def test_entry_symlink_hardlink_permissions_or_foreign_owner_refuse(self):
        p = self.module / "settings.sha256"
        original = p.read_bytes()
        p.rename(self.d / "sidecar")
        p.symlink_to(self.d / "sidecar")
        self.run_entry("10123", ok=False)
        p.unlink()
        p.write_bytes(original)
        p.chmod(0o644)
        os.link(p, self.d / "hardlink")
        self.run_entry("10123", ok=False)
        (self.d / "hardlink").unlink()
        p.chmod(0o666)
        self.run_entry("10123", ok=False)
        p.chmod(0o644)
        self.run_entry("10123", ok=False, env=dict(self.env, FIX_UNSAFE_OWNER=str(p)))
        self.no_bridge()

    def test_module_identity_and_apk_pair_are_verified(self):
        for name, changed in (
            ("module.prop", "id=other\n"),
            ("settings.apk", "changed"),
            ("settings.sha256", "f" * 64 + "\n"),
            ("settings.sha256", "a\n" * 32),
        ):
            p = self.module / name
            original = p.read_bytes()
            p.write_text(changed)
            self.run_entry("10123", ok=False)
            p.write_bytes(original)
        (self.d / "installed.apk").write_bytes(b"another-app")
        self.run_entry("10123", ok=False)
        self.no_bridge()

    def test_invalid_socket_is_not_reported_as_connected(self):
        for value in (
            "wrong",
            SOCKET + "\nforeign",
            SOCKET.upper(),
            SOCKET.replace("-89ab-", "--9ab-"),
        ):
            with self.subTest(socket=value):
                self.run_entry("10123", ok=False, env=dict(self.env, FIX_SOCKET=value))


if __name__ == "__main__":
    unittest.main()
