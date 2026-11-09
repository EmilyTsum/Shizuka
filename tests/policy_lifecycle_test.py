# SPDX-License-Identifier: GPL-2.0-only
"""Run the installed start/stop logic with real processes/locks and fake Android services."""

import hashlib
import json
import os
import pathlib
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))


class PolicyLifecycle(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.d = pathlib.Path(temporary.name)
        self.state = self.d / "state"
        self.state.mkdir(mode=0o700)
        (self.state / "active").touch()
        (self.d / "proc").mkdir()
        (self.d / "lib").mkdir()
        (self.d / "policy.dex").write_bytes(b"fixture DEX; never executed")
        (self.d / "policy.sha256").write_text(
            hashlib.sha256((self.d / "policy.dex").read_bytes()).hexdigest()
        )
        for name in ["config.sh", "runtime.sh", "policy.sh"]:
            shutil.copy2(ROOT / "module/lib" / name, self.d / "lib" / name)
        runtime = self.d / "lib/runtime.sh"
        # Some executors expose host-PID /proc with namespace PIDs in getpid(). Each
        # fixture records its /proc/self link before exec; identity checks still read
        # the actual kernel stat/cmdline of the same process across exec and exit.
        runtime.write_text(
            runtime.read_text().replace('"/proc/$1/stat"', '"/proc/$(fixture_pid "$1")/stat"')
            + """
ensure_busybox_shell() { :; } # The fixture applet wrapper is not an Android executable.
fixture_pid() {
 n=0
 while [ ! -f "$MODDIR/proc/$1" ] && [ "$n" -lt 50 ];do sleep 0.02;n=$((n+1));done
 cat "$MODDIR/proc/$1"
}
"""
        )
        policy = self.d / "lib/policy.sh"
        policy.write_text(
            policy.read_text().replace(
                '"/proc/$pid/cmdline"', '"/proc/$(fixture_pid "$pid")/cmdline"'
            )
        )
        config = self.d / "lib/config.sh"
        config.write_text(
            config.read_text().replace(
                "CONFIG_DIR=/data/adb/shizuka", 'CONFIG_DIR="$MODDIR/settings"'
            )
            + "\nrequire_qhs_backend() { :; }\n"
        )
        (self.d / "settings").mkdir(mode=0o700)
        (self.d / "settings/config.conf").write_text("schema=2\nclassic_qhs_host_aware=true\n")
        (self.d / "config.sh").write_text(
            (ROOT / "module/config.sh")
            .read_text()
            .replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        )
        (self.d / "bb").write_text(
            f"#!{sys.executable}\n"
            + """
import fcntl, os, pathlib, sys
root=pathlib.Path(__file__).parent
(root/'proc'/str(os.getpid())).write_text(os.readlink('/proc/self'))
a=sys.argv[1:]
if a[:3]==['stat','-c','%u']: print('0')
elif a[0]=='flock': fcntl.flock(int(a[1]),fcntl.LOCK_EX)
else:
    if a[0]=='setsid': os.setsid();os.execvp(a[1],a[1:])
    bb=os.environ.get('SHIZUKA_TEST_BUSYBOX')
    if bb: os.execv(bb,[bb]+a)
    os.execvp(a[0],a)
"""
        )
        (self.d / "app_process").write_text(
            f"#!{sys.executable}\n"
            + """
import json, os, pathlib, signal, sys, time
root=pathlib.Path(__file__).parent
(root/'proc'/str(os.getpid())).write_text(os.readlink('/proc/self'))
def event(name,**fields):
    with (root/'events').open('a') as stream:
        stream.write(json.dumps(dict(event=name,pid=os.getpid(),**fields))+'\\n')
action=sys.argv[5]
if action=='validate': sys.exit(0)
if action=='value': print('false' if (root/'disabled').exists() else 'true');sys.exit(0)
if action!='watch': raise SystemExit('unexpected Android operation')
event('watch',open_fds=[fd for fd in (6,9) if pathlib.Path('/proc/self/fd',str(fd)).exists()])
if (root/'fail-start').exists(): sys.exit(23)
def stop(*args):
    event('term')
    while (root/'hold-stop').exists(): time.sleep(0.02)
    event('exit')
    sys.exit(0)
signal.signal(signal.SIGTERM,stop)
print('extension_watch=ready; device_preferences=android',flush=True)
while True: time.sleep(1)
"""
        )
        for name in ["bb", "app_process"]:
            (self.d / name).chmod(0o755)
        (self.d / "id").write_text("#!/bin/sh\necho 0\n")
        (self.d / "id").chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.d) + os.pathsep + os.environ["PATH"])
        self.addCleanup(self.stop_fixtures)

    def events(self):
        path = self.d / "events"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def stop_fixtures(self):
        (self.d / "hold-stop").unlink(missing_ok=True)
        for pid in {event["pid"] for event in self.events() if event["event"] == "watch"}:
            mapping = self.d / "proc" / str(pid)
            if not mapping.exists():
                continue
            record = pathlib.Path("/proc", mapping.read_text(), "stat")
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 5
            while record.exists() and time.monotonic() < deadline:
                if record.read_text().rsplit(") ", 1)[1].split()[0] in {"Z", "X"}:
                    break
                time.sleep(0.01)

    def close_parent(self, process):
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)

    def launch(self, *arguments):
        process = subprocess.Popen(
            SHELL
            + [
                "-c",
                'exec 9>"$1/state/outer.lock";shift;exec "$@"',
                "policy-test",
                str(self.d),
                *SHELL,
                str(self.d / "config.sh"),
                *arguments,
            ],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(self.close_parent, process)
        return process

    def lifecycle(self, body):
        prelude = (
            "set -eu\n"
            f'MODDIR={shlex.quote(str(self.d))};STATE="$MODDIR/state";BB="$MODDIR/bb"\n'
            'fail() { echo "$*" >&2;exit 1; }\n'
            '. "$MODDIR/lib/runtime.sh"\n. "$MODDIR/lib/config.sh"\n. "$MODDIR/lib/policy.sh"\n'
            "lock_lifecycle\n"
        )
        process = subprocess.Popen(
            SHELL + ["-c", prelude + body],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(self.close_parent, process)
        return process

    def finish(self, process, *, ok=True):
        out, err = process.communicate(timeout=15)
        if ok:
            self.assertEqual(process.returncode, 0, out + err)
        else:
            self.assertNotEqual(process.returncode, 0, out + err)
        return out + err

    def await_event(self, name):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if any(event["event"] == name for event in self.events()):
                return
            time.sleep(0.01)
        self.fail("missing fixture event: " + name)

    def test_concurrent_starts_launch_once_and_detached_worker_releases_locks(self):
        starts = [self.launch("start") for _ in range(4)]
        for process in starts:
            self.finish(process)
        watchers = [event for event in self.events() if event["event"] == "watch"]
        self.assertEqual(len(watchers), 1)
        self.assertEqual(watchers[0]["open_fds"], [])
        self.finish(self.launch("stop"))
        self.assertFalse((self.state / "policy.pid").exists())
        self.assertTrue(any(event["event"] == "exit" for event in self.events()))

    def test_restart_waits_for_old_watcher_to_exit(self):
        self.finish(self.launch("start"))
        (self.d / "hold-stop").touch()
        stopping = self.launch("stop")
        restarting = None
        try:
            self.await_event("term")
            restarting = self.launch("start")
            with self.assertRaises(subprocess.TimeoutExpired):
                restarting.communicate(timeout=0.2)
            self.assertEqual(sum(event["event"] == "watch" for event in self.events()), 1)
        finally:
            (self.d / "hold-stop").unlink(missing_ok=True)
            self.finish(stopping)
            if restarting:
                self.finish(restarting)
        events = [event["event"] for event in self.events()]
        self.assertEqual(events, ["watch", "term", "exit", "watch"])

    def test_manual_start_waits_for_recovery_and_rechecks_active_state(self):
        recovery = self.lifecycle(
            'touch "$MODDIR/recovery-entered"\n'
            'while [ ! -f "$MODDIR/recovery-release" ];do sleep 0.02;done\n'
            'rm -f "$STATE/active"\n'
        )
        starting = None
        try:
            deadline = time.monotonic() + 5
            while not (self.d / "recovery-entered").exists():
                if time.monotonic() >= deadline:
                    self.fail("recovery fixture did not acquire lifecycle lock")
                time.sleep(0.01)
            starting = self.launch("start")
            with self.assertRaises(subprocess.TimeoutExpired):
                starting.communicate(timeout=0.2)
            self.assertEqual(self.events(), [])
        finally:
            (self.d / "recovery-release").touch()
            self.finish(recovery)
            if starting:
                self.assertIn("module not active", self.finish(starting, ok=False))
        self.assertEqual(self.events(), [])

    def test_control_starts_worker_under_its_existing_lifecycle_lock(self):
        control = (ROOT / "module/control.sh").read_text()
        helper = control[control.index("start_policy()") : control.index("keep_session()")]
        self.finish(self.lifecycle(helper + "\nstart_policy\n"))
        self.assertEqual([event["event"] for event in self.events()], ["watch"])

    def test_failed_start_reports_failure_and_removes_identity_records(self):
        (self.d / "fail-start").touch()
        self.assertIn("watcher", self.finish(self.launch("start"), ok=False))
        self.assertFalse((self.state / "policy.pid").exists())
        self.assertFalse((self.state / "policy.start").exists())

    def test_failed_identity_record_never_starts_android_policy(self):
        runtime = self.d / "lib/runtime.sh"
        with runtime.open("a") as stream:
            stream.write("\nrecord_process() { return 23; }\n")
        self.assertIn("cannot record", self.finish(self.launch("start"), ok=False))
        self.assertEqual(self.events(), [])

    def test_stop_ignores_an_unrelated_process_and_works_without_dex(self):
        unrelated = subprocess.Popen([str(self.d / "bb"), "sleep", "30"], env=self.env)
        try:
            deadline = time.monotonic() + 5
            mapping = self.d / "proc" / str(unrelated.pid)
            while not mapping.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            stat = pathlib.Path("/proc", mapping.read_text(), "stat").read_text()
            (self.state / "policy.pid").write_text(str(unrelated.pid))
            (self.state / "policy.start").write_text(stat.rsplit(") ", 1)[1].split()[19])
            (self.d / "policy.dex").unlink()
            self.finish(self.launch("stop"))
            self.assertIsNone(unrelated.poll())
            self.assertFalse((self.state / "policy.pid").exists())
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=5)

    def test_disabling_extension_stops_existing_worker(self):
        self.finish(self.launch("start"))
        (self.d / "disabled").touch()
        self.finish(self.launch("start"))
        self.assertFalse((self.state / "policy.pid").exists())
        self.assertEqual([event["event"] for event in self.events()], ["watch", "term", "exit"])

    def test_legacy_pid_only_watcher_is_replaced(self):
        self.finish(self.launch("start"))
        (self.state / "policy.start").unlink()
        self.finish(self.launch("start"))
        self.assertEqual(
            [event["event"] for event in self.events()], ["watch", "term", "exit", "watch"]
        )
        self.assertTrue((self.state / "policy.start").is_file())

    def test_stop_retains_records_when_a_matching_worker_has_another_identity(self):
        self.finish(self.launch("start"))
        (self.state / "policy.start").write_text("different-start-time")
        self.assertIn("identity differs", self.finish(self.launch("stop"), ok=False))
        self.assertTrue((self.state / "policy.pid").is_file())
        self.assertEqual((self.state / "policy.start").read_text(), "different-start-time")
        self.assertEqual([event["event"] for event in self.events()], ["watch"])


if __name__ == "__main__":
    unittest.main()
