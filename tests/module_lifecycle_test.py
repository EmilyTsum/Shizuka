# SPDX-License-Identifier: GPL-2.0-only
"""Exercise installed shell helpers with host fixtures; never mount or contact Android."""

import json
import os
import pathlib
import selectors
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))
CONTROL = (ROOT / "module/control.sh").read_text()
RECOVERY = CONTROL[CONTROL.index("qhs_features()") : CONTROL.index('case "${1:-status}"')]


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.d = pathlib.Path(self.tmp.name)
        self.state = self.d / "state"
        self.state.mkdir()
        self.bb = self.d / "bb"
        self.bb.write_text(
            f"#!{sys.executable}\n"
            + """import fcntl, hashlib, os, pathlib, sys
a = sys.argv[1:]
if a[:3] == ["stat", "-c", "%u"]:
    print(os.environ.get("TEST_OWNER", "0"))
elif a[0] == "flock":
    # Use a real inherited-FD lock, also on hosts without the flock utility.
    fcntl.flock(int(a[1]), fcntl.LOCK_EX)
else:
    bb = os.environ.get("SHIZUKA_TEST_BUSYBOX")
    if bb: os.execv(bb, [bb] + a)
    if a[0] == "sha256sum":
        print(hashlib.sha256(pathlib.Path(a[1]).read_bytes()).hexdigest() + "  " + a[1])
        sys.exit(0)
    if a[0] == "setsid":
        os.setsid()
        a = a[1:]
    os.execvp(a[0], a)
"""
        )
        self.bb.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.d) + os.pathsep + os.environ["PATH"])
        self.prelude = (
            "set -eu\n"
            f'MODDIR={shlex.quote(str(self.d))}; STATE="$MODDIR/state"; BB="$MODDIR/bb"\n'
            f". {shlex.quote(str(ROOT / 'module/lib/runtime.sh'))}\n"
            f". {shlex.quote(str(ROOT / 'module/profiles/samsung-sm-s931z-dzif.sh'))}\n"
            'fail() { echo "$*" >&2; exit 1; }\n'
        )

    def run_shell(self, body, *, ok=True):
        result = subprocess.run(
            SHELL + ["-c", self.prelude + body],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=5,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def stat_fixture(self, start="1234", comm="Bluetooth (worker) name", status="S"):
        # Fields after comm: state is 3, starttime is 22.
        fields = [status] + ["0"] * 18 + [start] + ["0"] * 8
        (self.d / "stat").write_text("4242 (" + comm + ") " + " ".join(fields) + "\n")
        return 'cat() { if [ "$1" = /proc/4242/stat ]; then command cat "$MODDIR/stat"; else command cat "$@"; fi; }\n'

    def test_process_start_handles_parentheses_and_spaces(self):
        result = self.run_shell(self.stat_fixture() + "process_start 4242")
        self.assertEqual(result.stdout, "1234\n")

    def test_process_start_matches_real_proc_record(self):
        pid = os.getpid()
        stat = pathlib.Path(f"/proc/{pid}/stat")
        if not stat.is_file():
            self.skipTest("executor does not expose numeric /proc records")
        expected = stat.read_text().rsplit(") ", 1)[1].split()[19]
        self.assertEqual(self.run_shell(f"process_start {pid}").stdout.strip(), expected)

    def test_dead_or_invalid_process_has_no_identity(self):
        for status, start in [("Z", "1234"), ("X", "1234"), ("S", "bad")]:
            with self.subTest(status=status, start=start):
                self.run_shell(
                    self.stat_fixture(start=start, status=status) + "process_start 4242", ok=False
                )
        for pid in ["0", "missing", "../self"]:
            self.run_shell(f"process_start {shlex.quote(pid)}", ok=False)

    def test_pid_reuse_rejected_by_saved_start_time(self):
        (self.state / "bt.pid").write_text("4242\n")
        (self.state / "bt.start").write_text("1234\n")
        self.run_shell(self.stat_fixture(start="5678") + "saved_process_matches bt 4242", ok=False)
        self.run_shell(self.stat_fixture() + "saved_process_matches bt 4242")

    def test_legacy_pid_record_can_still_be_checked(self):
        pid = os.getpid()
        (self.state / "bt.pid").write_text(str(pid))
        self.run_shell(f"saved_process_matches bt {pid}")
        self.run_shell(f"saved_process_matches bt {pid + 1}", ok=False)

    def test_record_process_publishes_pid_and_start(self):
        self.run_shell(self.stat_fixture() + "record_process bt 4242")
        self.assertEqual((self.state / "bt.pid").read_text(), "4242\n")
        self.assertEqual((self.state / "bt.start").read_text(), "1234\n")

    def test_empty_or_old_token_does_not_match(self):
        (self.state / "armed").write_text("new\n")
        self.run_shell('token_matches "$STATE/armed" new')
        for value in ["old", ""]:
            self.run_shell('token_matches "$STATE/armed" ' + shlex.quote(value), ok=False)

    def pending_record(self, name):
        path = self.state / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("record\n" if name == "mounts.tsv" else "")
        return path

    def test_recovery_pending_includes_orphaned_mount_and_qhs_records(self):
        self.run_shell("recovery_pending", ok=False)
        for name in ["active", "armed", "mounts.tsv", "qhs-nvm/intent", "qhs-nvm/restoring"]:
            with self.subTest(record=name):
                path = self.pending_record(name)
                self.run_shell("recovery_pending")
                path.unlink()
                self.run_shell("recovery_pending", ok=False)
        (self.state / "mounts.tsv").touch()
        self.run_shell("recovery_pending", ok=False)

    def test_activation_refuses_to_overwrite_unfinished_recovery(self):
        body = CONTROL.split("\n activate)\n", 1)[1]
        journal = '  : > "$STATE/mounts.tsv"'
        prefix = body.split(journal, 1)[0] + journal + "\n"
        (self.d / "proofs").mkdir()
        (self.d / "proofs/offline-ready").touch()
        (self.d / "inputs.sha256").touch()
        checks = """
check_inputs() { touch "$MODDIR/checked"; }
check_payloads() { :; }
host_zygote() { echo 4242; }
"""
        for name in ["mounts.tsv", "qhs-nvm/intent", "qhs-nvm/restoring"]:
            with self.subTest(record=name):
                path = self.pending_record(name)
                original = path.read_text()
                result = self.run_shell(checks + prefix, ok=False)
                self.assertIn("unfinished recovery", result.stderr)
                self.assertEqual(path.read_text(), original)
                self.assertFalse((self.d / "checked").exists())
                path.unlink()

    def test_status_reports_unfinished_recovery_without_mutating_it(self):
        body = CONTROL.split("\n status)\n", 1)[1].split("\n preflight)", 1)[0]
        script = (
            'check_inputs() { :; }\nlog() { echo "$*"; }\n'
            + "case status in status)\n"
            + body
            + "\nesac\n"
        )
        for name in ["armed", "mounts.tsv", "qhs-nvm/intent", "qhs-nvm/restoring"]:
            with self.subTest(record=name):
                path = self.pending_record(name)
                result = self.run_shell(script, ok=False)
                self.assertIn("recovery pending", result.stderr)
                self.assertTrue(path.exists())
                path.unlink()
        self.assertIn("stock hashes match; inactive", self.run_shell(script).stdout)

    def recovery_entry_fixture(self, filename):
        (self.d / "lib").mkdir()
        (self.d / "lib/runtime.sh").write_text((ROOT / "module/lib/runtime.sh").read_text())
        entry = self.d / filename
        entry.write_text(
            (ROOT / "module" / filename)
            .read_text()
            .replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        )
        (self.d / "control.sh").write_text("""
MODDIR=${0%/*}
printf '%s\n' "$1" >> "$MODDIR/calls"
case "$1" in
 rollback) rm -f "$MODDIR/state/active" "$MODDIR/state/armed" "$MODDIR/state/mounts.tsv" "$MODDIR/state/qhs-nvm/intent" "$MODDIR/state/qhs-nvm/restoring";;
 activate) touch "$MODDIR/state/active";;
esac
""")
        return entry

    def test_action_restores_orphaned_state_before_starting_another_trial(self):
        entry = self.recovery_entry_fixture("action.sh")
        calls = self.d / "calls"
        for name in ["mounts.tsv", "qhs-nvm/intent", "qhs-nvm/restoring"]:
            with self.subTest(record=name):
                path = self.pending_record(name)
                result = subprocess.run(
                    SHELL + [str(entry)], env=self.env, capture_output=True, text=True, timeout=5
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    calls.read_text().splitlines(), ["rollback", "preflight", "activate", "status"]
                )
                self.assertFalse(path.exists())
                (self.state / "active").unlink()
                calls.unlink()
        # An already healthy active generation keeps the existing idempotent Action behavior.
        (self.state / "active").touch()
        result = subprocess.run(
            SHELL + [str(entry)], env=self.env, capture_output=True, text=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls.read_text().splitlines(), ["status"])

    def test_boot_worker_restores_orphaned_state_before_reactivation(self):
        entry = self.recovery_entry_fixture("boot-activate.sh")
        with (self.d / "lib/runtime.sh").open("a") as runtime:
            runtime.write("""
host_zygote() { echo 4242; }
host_bt() { echo 4343; }
process_start() { echo 1234; }
sleep() { :; }
cat() { if [ "$1" = /proc/sys/kernel/random/boot_id ]; then echo test-boot; else command cat "$@"; fi; }
""")
        getprop = self.d / "getprop"
        getprop.write_text("#!/bin/sh\necho 1\n")
        getprop.chmod(0o755)
        (self.d / "proofs").mkdir()
        (self.d / "payloads.sha256").write_text("same generation\n")
        (self.d / "proofs/verified-payloads.sha256").write_text("same generation\n")
        calls = self.d / "calls"
        for name in ["mounts.tsv", "qhs-nvm/intent", "qhs-nvm/restoring"]:
            with self.subTest(record=name):
                path = self.pending_record(name)
                result = subprocess.run(
                    SHELL + [str(entry), "manual"],
                    env=self.env,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    calls.read_text().splitlines(),
                    ["rollback", "preflight", "activate", "keep-verified"],
                )
                self.assertFalse(path.exists())
                (self.state / "active").unlink()
                calls.unlink()

    def active_fixture(self):
        files = [
            ("4242", "libbluetooth_jni.so", "patched-libbluetooth_jni.so"),
            ("4242", "Bluetooth.apk", "patched-Bluetooth.apk"),
            (
                "4343",
                "libsehbluetooth_audio_session_aidl.so",
                "patched-libsehbluetooth_audio_session_aidl.so",
            ),
        ]
        (self.d / "payload").mkdir()
        for pid, live_name, payload_name in files:
            live = self.d / "live" / pid / live_name
            live.parent.mkdir(parents=True, exist_ok=True)
            live.write_text(payload_name)
            (self.d / "payload" / payload_name).write_text(payload_name)
        for name, pid, start in [("bt", "4242", "100"), ("hal", "4343", "200")]:
            (self.state / (name + ".pid")).write_text(pid)
            (self.state / (name + ".start")).write_text(start)
        return """
NATIVE=/stock/libbluetooth_jni.so; APK=/stock/Bluetooth.apk
HAL=/stock/libsehbluetooth_audio_session_aidl.so
check_payloads() { :; }
host_bt() { echo 4242; }
getprop() { echo 4343; }
process_start() { case "$1" in 4242) echo 100;;4343) echo 200;;*) return 1;;esac; }
nsrun() { "$BB" sha256sum "$MODDIR/live/$1/${4##*/}"; }
"""

    def test_active_verification_checks_both_processes_and_all_three_files(self):
        fixture = self.active_fixture()
        result = self.run_shell(fixture + 'verify_active_payloads\necho "$live_bt $live_hal"')
        self.assertEqual(result.stdout, "4242 4343\n")
        for file in (self.d / "live").rglob("*"):
            if not file.is_file():
                continue
            with self.subTest(file=file.name):
                original = file.read_text()
                file.write_text("wrong generation")
                result = self.run_shell(fixture + "verify_active_payloads", ok=False)
                self.assertIn("NOT APPLIED: /stock/" + file.name, result.stderr)
                file.write_text(original)

    def test_active_verification_rejects_reused_bluetooth_and_hal_pids(self):
        fixture = self.active_fixture()
        for name, process in [("bt", "Bluetooth"), ("hal", "audio HAL")]:
            with self.subTest(process=process):
                record = self.state / (name + ".start")
                original = record.read_text()
                record.write_text("old-process")
                result = self.run_shell(fixture + "verify_active_payloads", ok=False)
                self.assertIn("STALE: " + process, result.stderr)
                record.write_text(original)

    def test_watchers_send_guarded_recovery_requests(self):
        (self.d / "lib").mkdir()
        runtime = (ROOT / "module/lib/runtime.sh").read_text()
        (self.d / "lib/runtime.sh").write_text(
            runtime + "\nprocess_start() { echo new-process; }\n"
        )
        (self.d / "control.sh").write_text('printf "%s\\n" "$@" > "${0%/*}/request"\n')
        (self.d / "getprop").write_text("#!/bin/sh\nexit 0\n")
        (self.d / "getprop").chmod(0o755)
        (self.state / "active").touch()
        (self.state / "bt.pid").write_text("4242")
        (self.state / "bt.start").write_text("old-process")
        for filename, args, mode in [
            ("watchdog.sh", [str(self.d), "60", "current"], "trial"),
            ("health.sh", [str(self.d), "4242", "old-process", "current"], "health"),
        ]:
            source = (ROOT / "module" / filename).read_text()
            source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="${MODDIR}/bb"')
            entry = self.d / filename
            entry.write_text(source)
            marker = self.state / ("armed" if mode == "trial" else "health-token")
            marker.write_text("current")
            result = subprocess.run(
                SHELL + [str(entry), *args], env=self.env, capture_output=True, text=True, timeout=3
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            request = self.d / "request"
            self.assertEqual(request.read_text(), "rollback-" + mode + "\ncurrent\n")
            request.unlink()
            marker.write_text("other-generation")
            result = subprocess.run(
                SHELL + [str(entry), *args], env=self.env, capture_output=True, text=True, timeout=3
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(request.exists())
        self.assertFalse((self.d / ".no-auto").exists())

    def recovery_fixture(self):
        (self.state / "active").touch()
        (self.state / "armed").write_text("new\n")
        (self.state / "health-token").write_text("new\n")
        qhs = self.state / "qhs-nvm"
        qhs.mkdir()
        (qhs / "intent").touch()
        # A recovery that reaches mutations fails before any device operation.
        (self.d / "qhs-nvm.sh").write_text('touch "${0%/*}/recovery-attempted"; exit 23\n')

    def test_stale_trial_and_health_requests_have_no_effect(self):
        self.recovery_fixture()
        for mode in ["trial", "health"]:
            self.run_shell(RECOVERY + f"\nlock_lifecycle\nrollback {mode} old")
            self.assertFalse((self.d / "recovery-attempted").exists())
            self.assertFalse((self.d / ".no-auto").exists())
            self.assertTrue((self.state / "health-token").exists())
            self.assertEqual((self.state / "armed").read_text(), "new\n")

    def test_current_recovery_failure_leaves_trial_armed(self):
        self.recovery_fixture()
        self.run_shell(RECOVERY + "\nlock_lifecycle\nrollback trial new", ok=False)
        self.assertTrue((self.d / "recovery-attempted").exists())
        self.assertTrue((self.state / "armed").exists())
        self.assertFalse((self.d / ".no-auto").exists())

    def test_current_health_recovery_disables_auto_entry(self):
        self.recovery_fixture()
        self.run_shell(RECOVERY + "\nlock_lifecycle\nrollback health new", ok=False)
        self.assertTrue((self.d / ".no-auto").exists())
        self.assertTrue((self.state / "armed").exists())

    def test_disarmed_trial_and_inactive_health_requests_are_ignored(self):
        self.recovery_fixture()
        (self.state / "armed").unlink()
        (self.state / "active").unlink()
        for mode in ["trial", "health"]:
            self.run_shell(RECOVERY + f"\nlock_lifecycle\nrollback {mode} new")
        self.assertFalse((self.d / "recovery-attempted").exists())
        self.assertFalse((self.d / ".no-auto").exists())

    def test_queued_recovery_checks_token_after_lifecycle_lock(self):
        self.recovery_fixture()
        holder = subprocess.Popen(
            SHELL + ["-c", self.prelude + "lock_lifecycle\necho held\nread -r release"],
            env=self.env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        waiter = None
        try:
            self.assert_ready(holder, "held")
            waiter = subprocess.Popen(
                SHELL
                + [
                    "-c",
                    self.prelude + RECOVERY + "\necho waiting\nlock_lifecycle\nrollback trial new",
                ],
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assert_ready(waiter, "waiting")
            with self.assertRaises(subprocess.TimeoutExpired):
                waiter.communicate(timeout=0.15)
            # Simulate the current operation keeping the trial before release.
            (self.state / "armed").unlink()
            holder.communicate("release\n", timeout=3)
            out, err = waiter.communicate(timeout=3)
            self.assertEqual(waiter.returncode, 0, out + err)
            self.assertFalse((self.d / "recovery-attempted").exists())
        finally:
            for process in [holder, waiter]:
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=3)

    def assert_ready(self, process, expected):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(timeout=3), "shell readiness deadline expired")
        self.assertEqual(process.stdout.readline().strip(), expected)

    def test_unsafe_state_and_lock_symlinks_are_rejected(self):
        target = self.d / "target"
        target.mkdir()
        self.state.rmdir()
        self.state.symlink_to(target, target_is_directory=True)
        self.run_shell("lock_lifecycle", ok=False)
        self.state.unlink()
        self.state.mkdir()
        (self.state / "lifecycle.lock").symlink_to(target / "lock")
        self.run_shell("lock_lifecycle", ok=False)
        self.assertFalse((target / "lock").exists())

    def test_detached_workers_do_not_inherit_lifecycle_lock(self):
        (self.state / "bt.pid").write_text("4242\n")
        (self.d / "check_fd.py").write_text(
            'import os, pathlib, sys\ntry:\n os.fstat(9); result="inherited"\n'
            'except OSError:\n result="closed"\npathlib.Path(sys.argv[1]).write_text(result+"\\n")\n'
        )
        worker = (
            f'exec {shlex.quote(sys.executable)} "${{0%/*}}/check_fd.py" "${{0%/*}}/fd-check"\n'
        )
        for name in ["health.sh", "watchdog.sh", "config.sh"]:
            (self.d / name).write_text(worker)
        health = CONTROL[CONTROL.index("start_health()") : CONTROL.index("start_policy()")]
        watchdog = next(
            line for line in CONTROL.splitlines() if "setsid" in line and '/watchdog.sh"' in line
        )
        policy = next(line for line in CONTROL.splitlines() if '/config.sh" start' in line)
        for launch in [health + "\nprocess_start() { echo 1234; }\nstart_health", watchdog, policy]:
            with self.subTest(launch=launch.splitlines()[-1]):
                self.run_shell("lock_lifecycle\ntoken=test; trial_seconds=60\n" + launch + "\nwait")
                self.assertEqual((self.d / "fd-check").read_text(), "closed\n")

    def test_health_start_rejects_reused_pid_before_disarming(self):
        (self.state / "bt.pid").write_text("4242\n")
        (self.state / "bt.start").write_text("old-start\n")
        health = CONTROL[CONTROL.index("start_health()") : CONTROL.index("start_policy()")]
        self.run_shell(health + "\nprocess_start() { echo new-start; }\nstart_health", ok=False)
        self.assertFalse((self.state / "health-token").exists())

    def property_fixture(self):
        self.db = self.d / "properties.json"
        self.env["PROPERTY_DB"] = str(self.db)
        program = """#!/usr/bin/env python3
import json, os, pathlib, sys
p = pathlib.Path(os.environ["PROPERTY_DB"])
values = json.loads(p.read_text())
args = sys.argv[1:]
if pathlib.Path(sys.argv[0]).name == "getprop":
    if os.environ.get("FAIL_GETPROP"): sys.exit(23)
    if args: print(values.get(args[0], ""))
    else:
        for key, value in sorted(values.items()): print(f"[{key}]: [{value}]")
else:
    if os.environ.get("FAIL_RESETPROP"): sys.exit(23)
    if args[1] == "-d": values.pop(args[2], None)
    else: values[args[1]] = args[2]
    p.write_text(json.dumps(values))
"""
        for name in ["getprop", "resetprop"]:
            executable = self.d / name
            executable.write_text(program)
            executable.chmod(0o755)
        self.properties = (
            f". {shlex.quote(str(ROOT / 'module/lib/properties.sh'))}\n"
            'RESETPROP="$MODDIR/resetprop"\n'
        )
        return self.run_shell(self.properties + "managed_properties").stdout.splitlines()

    def test_properties_restore_exact_values_and_absence(self):
        entries = self.property_fixture()
        initial = {
            line.split()[1]: value
            for line, value in zip(entries, ["", "true", "stock with spaces", "disabled"])
        }
        initial[entries[-1].split()[1]] = "$(touch sentinel); literal"
        # A property's name inside another property's value is not its key.
        initial["unrelated.property"] = "preserved [" + entries[4].split()[1] + "]: [literal]"
        self.db.write_text(json.dumps(initial))
        self.run_shell(self.properties + "save_properties")
        for line in entries:
            saved = line.split()[0]
            self.run_shell(self.properties + f"set_managed_property {saved} module")
        self.run_shell(self.properties + "restore_properties")
        self.assertEqual(json.loads(self.db.read_text()), initial)
        self.assertFalse((self.d / "sentinel").exists())

    def test_legacy_property_snapshots_keep_previous_absence_semantics(self):
        entries = self.property_fixture()
        expected = {}
        for line in entries:
            saved, property_name, legacy = line.split()
            (self.state / saved).write_text("original\n")
            if legacy == "present":
                expected[property_name] = "original"
        # One of the previously tracked properties existed with an empty value.
        (self.state / "dual-mode-existed").touch()
        (self.state / "dual-mode").write_text("\n")
        expected[entries[1].split()[1]] = ""
        self.db.write_text(json.dumps({line.split()[1]: "module" for line in entries}))
        self.run_shell(self.properties + "restore_properties")
        self.assertEqual(json.loads(self.db.read_text()), expected)

    def test_default_profile_preserves_system_priorities_voice_and_logging(self):
        entries = self.property_fixture()
        initial = {line.split()[1]: "original" for line in entries}
        self.db.write_text(json.dumps(initial))
        (self.d / "payload").mkdir()
        (self.d / "payload/runtime.prop").write_text(
            "persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support=true\n"
        )
        config = f'. {shlex.quote(str(ROOT / "module/lib/config.sh"))}\nCONFIG_FILE="$MODDIR/missing.conf"; LEGACY_CONFIG="$MODDIR/also-missing.conf"\n'
        self.run_shell(self.properties + config + "save_properties\nprofile_apply_properties")
        current = json.loads(self.db.read_text())
        for saved, property_name, _ in (line.split() for line in entries):
            if saved not in {"samsung-cap", "r21"}:
                self.assertEqual(current[property_name], initial[property_name])
        self.assertEqual(
            set((self.state / "property-writes").read_text().splitlines()), {"samsung-cap", "r21"}
        )
        self.run_shell(self.properties + "restore_properties")
        self.assertEqual(json.loads(self.db.read_text()), initial)

    def test_recovery_preserves_later_android_property_changes(self):
        entries = self.property_fixture()
        initial = {line.split()[1]: "original" for line in entries}
        self.db.write_text(json.dumps(initial))
        self.run_shell(self.properties + "save_properties\nset_managed_property ad-priority 4001")
        changed = json.loads(self.db.read_text())
        changed[entries[5].split()[1]] = "7001"
        changed[entries[4].split()[1]] = "user-voice-choice"
        self.db.write_text(json.dumps(changed))
        self.run_shell(self.properties + "restore_properties")
        self.assertEqual(json.loads(self.db.read_text()), changed)

    def test_missing_or_invalid_property_write_journal_is_not_ignored(self):
        self.property_fixture()
        self.db.write_text("{}")
        self.run_shell(self.properties + "save_properties")
        (self.state / "property-writes").unlink()
        self.run_shell(self.properties + "restore_properties", ok=False)
        (self.state / "property-writes").write_text("unknown-property\n")
        self.run_shell(self.properties + "restore_properties", ok=False)
        (self.state / "property-writes").write_text("ad-priority\n")
        self.run_shell(self.properties + "restore_properties", ok=False)

    def test_unchanged_property_is_not_owned_and_duplicate_writes_are_rejected(self):
        entries = self.property_fixture()
        property_name = entries[5].split()[1]
        self.db.write_text(json.dumps({property_name: "4001"}))
        self.run_shell(self.properties + "save_properties\nset_managed_property ad-priority 4001")
        self.assertEqual((self.state / "property-writes").read_text(), "")
        self.run_shell(self.properties + "set_managed_property ad-priority 6001")
        self.run_shell(self.properties + "set_managed_property ad-priority 7001", ok=False)
        self.run_shell(self.properties + "restore_properties")
        self.assertEqual(json.loads(self.db.read_text()), {property_name: "4001"})

    def test_backend_with_no_property_overrides_restores_without_writes(self):
        self.property_fixture()
        self.db.write_text('{"system.preference": "user-value"}')
        self.run_shell(
            self.properties
            + "profile_managed_properties() { :; }\nsave_properties\nrestore_properties"
        )
        self.assertEqual(json.loads(self.db.read_text()), {"system.preference": "user-value"})

    def test_property_restore_failure_is_not_hidden(self):
        self.property_fixture()
        self.db.write_text("{}")
        (self.state / "r21").write_text("old\n")
        self.env["FAIL_RESETPROP"] = "1"
        self.run_shell(self.properties + "restore_properties", ok=False)

    def test_property_read_failure_does_not_publish_or_restore_snapshot(self):
        self.property_fixture()
        self.db.write_text("{}")
        self.env["FAIL_GETPROP"] = "1"
        self.run_shell(self.properties + "save_properties", ok=False)
        self.assertFalse((self.state / "properties-v1").exists())
        self.run_shell(self.properties + "restore_properties", ok=False)

    def test_health_start_failure_does_not_disarm_trial(self):
        (self.state / "active").touch()
        (self.state / "armed").write_text("trial\n")
        (self.state / "hci-and-audio-verified").touch()
        body = CONTROL.split("\n commit)\n", 1)[1].split("\n rollback)", 1)[0]
        self.run_shell(
            "verify_active_payloads() { :; }\nstart_health() { return 23; }\n"
            + "case commit in commit)\n"
            + body
            + "\nesac\n",
            ok=False,
        )
        self.assertTrue((self.state / "armed").exists())

    def test_all_module_scripts_have_valid_shell_syntax(self):
        for script in (ROOT / "module").rglob("*.sh"):
            result = subprocess.run(SHELL + ["-n", str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, str(script) + result.stderr)


if __name__ == "__main__":
    unittest.main()
