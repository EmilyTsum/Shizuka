# SPDX-License-Identifier: GPL-2.0-only
"""Run the native installer with fake Android boundaries, never an audio service."""

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))
APK = b"synthetic APK\x00stdin\n" + bytes(range(256)) * 17


class NativeAppInstall(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="shizuka-native-install-")
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name).resolve()
        self.bin_dir = self.d / "bin"
        self.bin_dir.mkdir()
        self.module = self.d / "data/adb/modules/s25_aptx_adaptive"
        self.calls = self.d / "calls.jsonl"
        self.installed = self.d / "package-manager/base.apk"
        self.env = dict(
            os.environ,
            PATH=str(self.bin_dir) + os.pathsep + os.environ["PATH"],
            FIX_ROOT=str(self.d),
            FIX_CALLS=str(self.calls),
            FIX_INSTALLED=str(self.installed),
            FIX_EXPECTED_APK=str(self.module / "settings.apk"),
        )
        self.tool("id", "print(os.environ.get('FIX_UID', '0'))\n")
        self.tool(
            "bb",
            """import hashlib
a = sys.argv[1:]
if a[:3] == ['stat', '-c', '%s']:
    print(pathlib.Path(a[3]).stat().st_size)
elif a[0] == 'sha256sum':
    print(hashlib.sha256(pathlib.Path(a[1]).read_bytes()).hexdigest() + '  ' + a[1])
elif a[0] == 'setsid':
    # Inspect descriptors before opening the fixture's event log.
    opened = []
    for fd in (8, 9):
        try:
            os.fstat(fd)
            opened.append(fd)
        except OSError:
            pass
    record('setsid', argv=a[1:], inherited=opened)
    os.execv(a[1], a[1:])
elif a[0] == 'timeout':
    assert a[1:6] == ['-s', 'TERM', '-k', '5', '60'], a
    record('installer_timeout', argv=a[1:])
    result = subprocess.run(a[6:], timeout=60, check=False)
    record('installer_finished', exit=result.returncode)
    sys.exit(result.returncode)
else:
    os.execvp(a[0], a)
""",
        )
        self.tool(
            "pm",
            """import hashlib
a = sys.argv[1:]
installed = pathlib.Path(os.environ['FIX_INSTALLED'])
if a == ['path', 'dev.shizuka.settings']:
    record('pm_path', argv=a, present=installed.is_file())
    if installed.is_file():
        print('package:' + str(installed))
    else:
        sys.exit(1)
elif len(a) == 4 and a[:3] == ['install', '-r', '-S']:
    body = sys.stdin.buffer.read()
    expected = pathlib.Path(os.environ['FIX_EXPECTED_APK']).read_bytes()
    assert a[3] == str(len(expected)) and body == expected, 'not an exact stdin install'
    record('pm_install', argv=a, size=len(body), sha256=hashlib.sha256(body).hexdigest())
    gate = os.environ.get('FIX_INSTALL_GATE')
    if gate:
        record('pm_waiting')
        deadline = time.monotonic() + 5
        while not pathlib.Path(gate).exists():
            if time.monotonic() >= deadline:
                sys.exit(92)
            time.sleep(0.01)
    behavior = os.environ.get('FIX_PM_INSTALL', 'ok')
    if behavior == 'fail':
        print('Failure [synthetic package manager failure]', file=sys.stderr)
        sys.exit(1)
    if behavior != 'missing':
        installed.parent.mkdir(parents=True, exist_ok=True)
        installed.write_bytes(b'wrong installed APK' if behavior == 'wrong' else body)
    print('Success')
else:
    record('forbidden', command='pm', argv=a)
    sys.exit(91)
""",
        )
        for command in ("am", "cmd", "su", "setprop", "getprop", "mount", "nsenter"):
            self.tool(
                command,
                "record('forbidden', command=pathlib.Path(sys.argv[0]).name, argv=sys.argv[1:])\n"
                "sys.exit(91)\n",
            )
        self.tool(
            "event",
            """opened = []
for fd in (8, 9):
    try:
        os.fstat(fd)
        opened.append(fd)
    except OSError:
        pass
record(sys.argv[1], argv=sys.argv[2:], inherited=opened)
""",
        )
        self.make_module(self.module)

    def tool(self, name, body):
        path = self.bin_dir / name
        path.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, subprocess, sys, time\n"
            "def record(kind, **fields):\n"
            "    line = json.dumps(dict(kind=kind, **fields)).encode() + b'\\n'\n"
            "    fd = os.open(os.environ['FIX_CALLS'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)\n"
            "    try: os.write(fd, line)\n"
            "    finally: os.close(fd)\n" + body
        )
        path.chmod(0o755)

    def make_module(self, directory):
        directory.mkdir(parents=True)
        (directory / "lib").mkdir()
        source = (ROOT / "module/settings.sh").read_text()
        self.assertEqual(source.count("BB=/data/adb/ksu/bin/busybox"), 1)
        (directory / "settings.sh").write_text(
            source.replace(
                "BB=/data/adb/ksu/bin/busybox", "BB=" + shlex.quote(str(self.bin_dir / "bb"))
            )
        )
        (directory / "settings.apk").write_bytes(APK)
        (directory / "settings.sha256").write_text(hashlib.sha256(APK).hexdigest() + "\n")
        (directory / "ui-lifecycle.sh").write_text('event ui_lifecycle "$0" "$@"\n')
        for name in ("settings-bridge.sh", "settings-bootstrap.sh", "control.sh", "apply.sh"):
            (directory / name).write_text('event forbidden "$0" "$@"\nexit 91\n')
        # The unrelated boot action is inert, not the real audio activator.
        (directory / "boot-activate.sh").write_text('event boot_boundary "$0" "$@"\n')
        (directory / "lib/runtime.sh").write_text(
            'ensure_busybox_shell() { event shell_boundary "$@"; }\n'
            'prepare_state() { mkdir -p "$STATE";chmod 700 "$STATE";event state_boundary; }\n'
        )
        service = (ROOT / "module/service.sh").read_text()
        self.assertEqual(service.count("BB=/data/adb/ksu/bin/busybox"), 1)
        self.assertEqual(service.count("/data/adb/modules/s25_aptx_adaptive"), 1)
        (directory / "service.sh").write_text(
            service.replace(
                "BB=/data/adb/ksu/bin/busybox", "BB=" + shlex.quote(str(self.bin_dir / "bb"))
            ).replace("/data/adb/modules/s25_aptx_adaptive", str(self.module))
        )

    def events(self):
        if not self.calls.exists():
            return []
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def wait_for(self, kind):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            values = [e for e in self.events() if e["kind"] == kind]
            if values:
                return values[-1]
            time.sleep(0.01)
        self.fail(f"Missing fixture event {kind}: {self.events()}")

    def run_install(self, ok=True, **options):
        result = subprocess.run(
            [*SHELL, str(self.module / "settings.sh"), "--install-only"],
            env=dict(self.env, **options),
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        self.assert_no_launch()
        return result

    def assert_no_launch(self):
        self.assertFalse([e for e in self.events() if e["kind"] == "forbidden"], self.events())

    def assert_exact_installed(self):
        self.assertEqual(self.installed.read_bytes(), APK)
        self.assertEqual(
            hashlib.sha256(self.installed.read_bytes()).hexdigest(),
            (self.module / "settings.sha256").read_text().strip(),
        )

    def run_service(self, directory=None, **options):
        directory = directory or self.module
        # Inherit real descriptors; the service must close both before setsid.
        command = (
            f"exec 8>{shlex.quote(str(self.d / 'fd8'))}; "
            f"exec 9>{shlex.quote(str(self.d / 'fd9'))}; "
            "exec " + shlex.join([*SHELL, str(directory / "service.sh")])
        )
        result = subprocess.run(
            [*SHELL, "-c", command],
            env=dict(self.env, **options),
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_fresh_package_is_installed_from_exact_binary_stdin(self):
        self.assertFalse(self.installed.exists())
        self.run_install()
        self.assert_exact_installed()
        self.assertEqual(
            [e["kind"] for e in self.events()],
            ["pm_path", "ui_lifecycle", "pm_install", "pm_path"],
        )
        install = self.events()[2]
        self.assertEqual(install["argv"], ["install", "-r", "-S", str(len(APK))])
        self.assertEqual(install["sha256"], hashlib.sha256(APK).hexdigest())
        self.assertEqual(self.events()[1]["argv"], [str(self.module / "ui-lifecycle.sh"), "start"])

    def test_matching_installed_package_is_not_reinstalled(self):
        self.installed.parent.mkdir()
        self.installed.write_bytes(APK)
        self.run_install()
        self.assert_exact_installed()
        self.assertEqual([e["kind"] for e in self.events()], ["pm_path", "ui_lifecycle"])

    def test_old_installed_package_is_replaced_and_reverified(self):
        self.installed.parent.mkdir()
        self.installed.write_bytes(b"old installed APK")
        self.run_install()
        self.assert_exact_installed()
        self.assertEqual([e["kind"] for e in self.events()].count("pm_path"), 2)
        self.assertEqual([e["kind"] for e in self.events()].count("pm_install"), 1)

    def test_package_manager_failure_does_not_launch_or_report_success(self):
        result = self.run_install(ok=False, FIX_PM_INSTALL="fail")
        self.assertIn("synthetic package manager failure", result.stderr)
        self.assertFalse(self.installed.exists())
        self.assertEqual([e["kind"] for e in self.events()].count("pm_path"), 1)

    def test_package_manager_success_is_not_trusted_without_matching_bytes(self):
        for behavior in ("wrong", "missing"):
            with self.subTest(behavior=behavior):
                self.installed.unlink(missing_ok=True)
                self.calls.unlink(missing_ok=True)
                self.run_install(ok=False, FIX_PM_INSTALL=behavior)
                self.assertEqual([e["kind"] for e in self.events()].count("pm_path"), 2)
                if self.installed.exists():
                    self.assertNotEqual(self.installed.read_bytes(), APK)

    def test_nonroot_or_invalid_bundle_cannot_reach_package_manager(self):
        self.run_install(ok=False, FIX_UID="2000")
        self.assertEqual(self.events(), [])
        (self.module / "settings.apk").write_bytes(b"tampered bundle")
        self.run_install(ok=False)
        self.assertEqual(self.events(), [])
        (self.module / "settings.apk").write_bytes(APK)
        (self.module / "settings.sha256").unlink()
        self.run_install(ok=False)
        self.assertEqual(self.events(), [])

    def test_disabled_or_removed_module_is_not_installed(self):
        for name in ("disable", "remove"):
            with self.subTest(marker=name):
                marker = self.module / name
                marker.touch()
                self.run_install(ok=False)
                self.assertEqual(self.events(), [])
                marker.unlink()

    def test_service_installer_is_async_bounded_and_closes_lifecycle_descriptors(self):
        gate = self.d / "release-installer"
        try:
            self.run_service(FIX_INSTALL_GATE=str(gate))
            self.wait_for("pm_waiting")
            self.assertFalse(
                self.installed.exists(), "service must return while installation is pending"
            )
            event = self.wait_for("installer_timeout")
            self.assertEqual(
                event["argv"],
                [
                    "-s",
                    "TERM",
                    "-k",
                    "5",
                    "60",
                    str(self.bin_dir / "bb"),
                    "sh",
                    str(self.module / "settings.sh"),
                    "--install-only",
                ],
            )
            install_start = [
                e for e in self.events() if e["kind"] == "setsid" and "timeout" in e["argv"]
            ]
            self.assertEqual(len(install_start), 1)
            self.assertEqual(install_start[0]["inherited"], [])
            # Positive control: descriptors really entered the service open.
            self.assertEqual(self.wait_for("shell_boundary")["inherited"], [8, 9])
            kinds = [event["kind"] for event in self.events()]
            self.assertLess(kinds.index("state_boundary"), kinds.index("installer_timeout"))
        finally:
            gate.touch()
            self.assertEqual(self.wait_for("installer_finished")["exit"], 0)
            self.wait_for("boot_boundary")
        self.assert_exact_installed()
        self.assert_no_launch()
        self.assertTrue((self.module / "state/settings-install.log").is_file())
        self.assertTrue((self.module / "state/boot-activate.log").is_file())

    def test_service_install_failure_is_logged_without_blocking_boot_boundary(self):
        self.run_service(FIX_PM_INSTALL="fail")
        self.assertNotEqual(self.wait_for("installer_finished")["exit"], 0)
        self.wait_for("boot_boundary")
        self.assertFalse(self.installed.exists())
        self.assertIn(
            "synthetic package manager failure",
            (self.module / "state/settings-install.log").read_text(),
        )
        self.assert_no_launch()

    def test_service_staging_directory_does_not_register_the_app(self):
        staging = self.d / "data/adb/modules_update/s25_aptx_adaptive"
        self.make_module(staging)
        self.run_service(staging)
        self.wait_for("boot_boundary")
        self.assertFalse(
            [
                e
                for e in self.events()
                if e["kind"] in ("pm_path", "pm_install", "installer_timeout")
            ]
        )
        self.assertFalse(self.installed.exists())
        self.assert_no_launch()

    def test_service_disabled_removed_or_missing_installer_does_not_register_app(self):
        for name in ("disable", "remove", "settings.sh"):
            with self.subTest(guard=name):
                marker = self.module / name
                if name == "settings.sh":
                    marker.rename(self.module / "settings-held.sh")
                else:
                    marker.touch()
                self.calls.unlink(missing_ok=True)
                self.run_service()
                self.wait_for("boot_boundary")
                self.assertFalse(
                    [
                        e
                        for e in self.events()
                        if e["kind"] in ("pm_path", "pm_install", "installer_timeout")
                    ]
                )
                self.assertFalse(self.installed.exists())
                self.assert_no_launch()
                if name == "settings.sh":
                    (self.module / "settings-held.sh").rename(marker)
                else:
                    marker.unlink()


if __name__ == "__main__":
    unittest.main()
