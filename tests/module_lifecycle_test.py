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
            + """import fcntl, hashlib, os, pathlib, stat, sys
a = sys.argv[1:]
if a[:3] == ["stat", "-c", "%u"]:
    print(os.environ.get("TEST_OWNER", "0"))
elif a[:3] == ["stat", "-c", "%u:%a"]:
    import stat
    print(os.environ.get("TEST_OWNER", "0") + ":" + format(stat.S_IMODE(pathlib.Path(a[3]).stat().st_mode), "o"))
elif a[:3] == ["stat", "-c", "%u:%a:%h"]:
    s=pathlib.Path(a[3]).stat()
    print(os.environ.get("TEST_OWNER", "0") + ":" + format(stat.S_IMODE(s.st_mode), "o") + ":" + str(s.st_nlink))
elif a[0] == "flock":
    # Use a real inherited-FD lock, also on hosts without the flock utility.
    fcntl.flock(int(a[1]), fcntl.LOCK_EX)
elif a[:6] == ["timeout", "-s", "TERM", "-k", "1", "5"]:
    os.execvp(a[6], a[6:])
else:
    if a[0] == "awk" and a[-1] == "/proc/222/maps" and os.environ.get("TEST_HAL_MAPS"):
        a[-1]=os.environ["TEST_HAL_MAPS"]
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
            f". {shlex.quote(str(ROOT / 'module/lib/policy.sh'))}\n"
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
        for name in ["active", "armed", "mounts.tsv", "qhs-nvm/intent", "qhs-nvm/restoring",
                     "latency-runtime.active", "latency-remote-uncertain", "latency-provider.pid",
                     "latency-provider.start", "dsp-trial.active"]:
            with self.subTest(record=name):
                path = self.pending_record(name)
                self.run_shell("recovery_pending")
                path.unlink()
                self.run_shell("recovery_pending", ok=False)
        (self.state / "mounts.tsv").touch()
        self.run_shell("recovery_pending", ok=False)

    def test_broken_latency_ownership_markers_still_fence_activation(self):
        for name in ["latency-runtime.active", "latency-remote-uncertain", "active", "armed", "mounts.tsv", "latency-provider.pid"]:
            with self.subTest(record=name):
                marker = self.state / name
                marker.symlink_to(self.state / "missing")
                self.run_shell("recovery_pending")
                marker.unlink()

    def application_fixture(self, *, unresolved=True):
        directory = self.state / "latency-application"
        directory.mkdir(mode=0o700)
        (directory / "history").write_text("Protected terminal history must remain\n")
        if unresolved:
            (directory / "unresolved").touch()
        helper = self.d / "latency-apply-fence.sh"
        helper.write_text('''#!/bin/sh
MODDIR=${0%/*}; STATE="$MODDIR/state"
printf '%s\n' "$1" >> "$MODDIR/fence-calls"
case "$1" in
 check|health) [ ! -e "$STATE/latency-application/unresolved" ];;
 reconcile)
  [ -e /dev/fd/9 ] || exit 91
  [ -f "$MODDIR/allow-reconcile" ] || exit 3
  rm -f "$STATE/latency-application/unresolved"
  ;;
 *) exit 92;;
esac
''')
        helper.chmod(0o644)
        return directory

    def no_audio_mutation(self):
        return '''
log() { printf '%s\\n' "$*"; }
check_inputs() { :; }
stop_policy() { touch "$MODDIR/forbidden-write";return 95; }
profile_stop_runtime() { touch "$MODDIR/forbidden-write";return 95; }
quiesce_radio() { touch "$MODDIR/forbidden-write";return 95; }
restore_properties() { touch "$MODDIR/forbidden-write";return 95; }
setprop() { touch "$MODDIR/forbidden-write";return 95; }
resume_radio() { touch "$MODDIR/forbidden-write";return 95; }
'''

    def physical_cleanup_fixture(self):
        (self.state / "active").touch()
        (self.state / "armed").write_text("current\n")
        (self.state / "mounts.tsv").write_text("999999 mnt:[1] /stock/unused\n")
        return '''
log() { printf '%s\\n' "$*"; }
event() { printf '%s\\n' "$1" >> "$MODDIR/physical-calls"; }
stop_policy() { event policy-stop; }
profile_stop_runtime() { event runtime-stop; }
qhs_pending() { return 1; }
quiesce_radio() { event radio-off; }
host_bt() { :; }
restore_properties() { event properties; }
getprop() { echo 222; }
profile_recover_experiments() { event remote-recover; }
setprop() { event hal-restart; }
sleep() { :; }
profile_hal_ready() { :; }
profile_finish_runtime_recovery() { event runtime-finalize; }
resume_radio() { event radio-resume; }
check_inputs() { :; }
verify_stock_recovery() { event independently-verified; }
'''

    def test_application_fence_is_pending_without_treating_terminal_history_as_physical(self):
        directory = self.application_fixture()
        self.run_shell("recovery_pending")
        self.run_shell("physical_recovery_pending", ok=False)
        (directory / "unresolved").unlink()
        self.run_shell("recovery_pending", ok=False)
        self.assertTrue((directory / "history").exists())

    def test_activation_commit_and_keep_fence_after_acquiring_lifecycle_lock(self):
        directory = self.application_fixture()
        entry = CONTROL[CONTROL.index("# Serialize activation") : CONTROL.index("quiesce_radio()")]
        helper = self.d / "latency-apply-fence.sh"
        helper.write_text(helper.read_text().replace('case "$1" in', '[ -e /dev/fd/9 ] || exit 91\ncase "$1" in'))
        for command in ("activate", "commit", "keep-verified"):
            with self.subTest(command=command):
                self.run_shell(f"set -- {command}\n" + entry + '\ntouch "$MODDIR/forbidden-write"', ok=False)
                self.assertFalse((self.d / "forbidden-write").exists())
        (directory / "unresolved").unlink()
        self.run_shell("set -- keep-verified\n" + entry + '\ntouch "$MODDIR/allowed-after-fence"')
        self.assertTrue((self.d / "allowed-after-fence").exists())

    def test_unsafe_reader_cannot_authorize_or_execute_an_application_check(self):
        self.application_fixture()
        helper = self.d / "latency-apply-fence.sh"
        helper.chmod(0o666)
        self.run_shell("application_fence check", ok=False)
        self.assertFalse((self.d / "fence-calls").exists())
        helper.chmod(0o644)
        (self.d / "linked-reader").hardlink_to(helper)
        self.run_shell("application_fence check", ok=False)
        self.assertFalse((self.d / "fence-calls").exists())

    def test_app_only_unknown_or_old_boot_never_replays_radio_or_other_user_state(self):
        directory = self.application_fixture()
        (directory / "old-boot").write_text("previous boot\n")
        (self.state / "radio-on").write_text("1\n")
        result = self.run_shell(RECOVERY + self.no_audio_mutation() + "\nlock_lifecycle\nrollback", ok=False)
        self.assertIn("no audio state was changed", result.stdout)
        self.assertFalse((self.d / "forbidden-write").exists())
        self.assertTrue((directory / "unresolved").exists())
        self.assertEqual((self.state / "radio-on").read_text(), "1\n")
        (self.d / "latency-apply-fence.sh").unlink()  # Missing reader stays fenced too.
        self.run_shell(RECOVERY + self.no_audio_mutation() + "\nlock_lifecycle\nrollback", ok=False)
        self.assertFalse((self.d / "forbidden-write").exists())
        self.assertTrue((directory / "old-boot").exists())

    def test_app_only_reconciliation_inherits_lock_without_touching_audio(self):
        directory = self.application_fixture()
        (self.d / "allow-reconcile").touch()
        self.run_shell(RECOVERY + self.no_audio_mutation() + "\nlock_lifecycle\nrollback")
        self.assertFalse((self.d / "forbidden-write").exists())
        self.assertFalse((directory / "unresolved").exists())
        self.assertIn("reconcile", (self.d / "fence-calls").read_text().splitlines())

    def test_late_application_failure_retires_only_verified_physical_markers_and_never_cycles_again(self):
        directory = self.application_fixture()
        fixture = self.physical_cleanup_fixture()
        first = self.run_shell(RECOVERY + fixture + "\nlock_lifecycle\nrollback", ok=False)
        self.assertIn("stock audio recovery verified", first.stdout)
        events = (self.d / "physical-calls").read_text()
        self.assertTrue(events.endswith("independently-verified\n"))
        self.assertFalse((self.state / "active").exists())
        self.assertFalse((self.state / "armed").exists())
        self.assertEqual((self.state / "mounts.tsv").read_text(), "")
        self.assertTrue((directory / "unresolved").exists())
        self.run_shell(RECOVERY + fixture + "\nlock_lifecycle\nrollback", ok=False)
        self.assertEqual((self.d / "physical-calls").read_text(), events)
        self.run_shell(RECOVERY + fixture + "\nlock_lifecycle\nrollback health current")
        self.assertEqual((self.d / "physical-calls").read_text(), events)
        self.assertFalse((self.d / ".no-auto").exists())
        self.run_shell("recovery_pending")
        self.run_shell("physical_recovery_pending", ok=False)

    def test_late_success_requires_physical_verification_before_receipt_completion(self):
        directory = self.application_fixture()
        fixture = self.physical_cleanup_fixture() + '\nverify_stock_recovery() { event independently-verified;touch "$MODDIR/allow-reconcile"; }\n'
        self.run_shell(RECOVERY + fixture + "\nlock_lifecycle\nrollback")
        self.assertFalse((directory / "unresolved").exists())
        self.assertEqual((self.d / "fence-calls").read_text().splitlines().count("reconcile"), 2)
        self.assertFalse((self.state / "armed").exists())

    def test_failed_independent_physical_proof_preserves_physical_markers_and_app_fence(self):
        directory = self.application_fixture()
        fixture = self.physical_cleanup_fixture() + '\nverify_stock_recovery() { event proof-failed;return 43; }\n'
        self.run_shell(RECOVERY + fixture + "\nlock_lifecycle\nrollback", ok=False)
        for name in ("active", "armed", "mounts.tsv"):
            self.assertTrue((self.state / name).exists())
        self.assertTrue((self.state / "mounts.tsv").read_text())
        self.assertTrue((directory / "unresolved").exists())
        self.assertEqual((self.d / "fence-calls").read_text().splitlines().count("reconcile"), 1)

    def stock_verification_fixture(self):
        stock = self.d / "stock"
        stock.mkdir()
        names = ("native.so", "hal.so", "Bluetooth.apk", "offload.xml", "framework.xml")
        paths = [stock / name for name in names]
        import hashlib
        (self.d / "inputs.sha256").write_text("".join(
            hashlib.sha256((name + " original").encode()).hexdigest() + " " + str(path) + "\n"
            for name, path in zip(names, paths)
        ))
        for name, path in zip(names, paths):
            path.write_text(name + " original")
        maps = self.d / "hal-maps"
        maps.write_text(str(paths[1]) + "\n/vendor/lib64/libar-gsl.so\n")
        self.env["TEST_HAL_MAPS"] = str(maps)
        for name, source in {
            "dumpsys": '''#!/bin/sh
printf 'binder %s\\n' "$2" >> "$MODDIR/stock-events"
[ "$2" != "${MISSING_BINDER:-}" ] || exit 17
echo 222
''',
            "service": '''#!/bin/sh
printf 'ping %s\\n' "$2" >> "$MODDIR/stock-events"
[ "$2" != "${MISSING_BINDER:-}" ] || exit 18
echo 'Result: Parcel(00000000)'
''',
        }.items():
            path = self.d / name
            path.write_text(source)
            path.chmod(0o755)
        return f'''
export MODDIR
NATIVE={shlex.quote(str(paths[0]))};HAL={shlex.quote(str(paths[1]))};APK={shlex.quote(str(paths[2]))}
OFFLOAD={shlex.quote(str(paths[3]))};FRAMEWORK={shlex.quote(str(paths[4]))}
next_hal=222;previous_hal=221
check_inputs() {{ :; }}
allowed() {{ profile_target_allowed "$1"; }}
host_zygote() {{ echo 111; }}
host_bt() {{ if [ "${{TEST_RADIO_OFF:-0}}" != 1 ];then echo 333;fi; }}
getprop() {{ echo 222; }}
process_start() {{ case "$1" in 111) echo 100;;222) echo 200;;333) echo 300;;*) return 1;;esac; }}
profile_latency_recovery_preflight() {{ [ "${{TEST_BAD_DSP:-0}}" != 1 ]; }}
profile_latency_dsp_state() {{ echo running; }}
cat() {{
 if [ "$1" = /proc/sys/kernel/random/boot_id ];then echo 11111111-2222-3333-abcd-555555555555
 else command cat "$@";fi
}}
nsrun() {{
 printf 'namespace %s %s\\n' "$1" "$4" >> "$MODDIR/stock-events"
 if [ "$4" = /vendor/lib64/libar-gsl.so ];then
  echo 4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc
 elif [ "$1:$4" = "${{TEST_BAD_NAMESPACE:-}}" ];then
  echo ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
 else "$BB" sha256sum "$4";fi
}}
'''

    def test_independent_stock_boundary_checks_all_three_binders_and_live_core_namespaces(self):
        fixture = self.stock_verification_fixture()
        body = RECOVERY + fixture + "\nverify_stock_recovery"
        self.run_shell(body)
        events = (self.d / "stock-events").read_text().splitlines()
        expected = ["android.hardware.audio.core.IModule/default", "android.hardware.audio.effect.IFactory/default",
                    "vendor.samsung.hardware.bluetooth.audio.IBluetoothAudioProviderFactory/default"]
        # Use the profile's exact Samsung factory spelling instead of a guessed alias.
        factory = self.run_shell('printf "%s" "$AUDIO_HAL_FACTORY"').stdout
        expected[-1] = factory
        for name in expected:
            self.assertIn("binder " + name, events)
            self.assertIn("ping " + name, events)
        for pid, name in ((1, "native.so"), (111, "native.so"), (333, "Bluetooth.apk"), (222, "hal.so")):
            self.assertIn(f"namespace {pid} {self.d}/stock/{name}", events)
        for name in expected:
            with self.subTest(missing_binder=name):
                self.env["MISSING_BINDER"] = name
                self.run_shell(body, ok=False)
        self.env.pop("MISSING_BINDER")
        for pid, name in ((111, "native.so"), (333, "Bluetooth.apk"), (222, "hal.so")):
            with self.subTest(bad_namespace=pid):
                self.env["TEST_BAD_NAMESPACE"] = f"{pid}:{self.d}/stock/{name}"
                self.run_shell(body, ok=False)
        self.env.pop("TEST_BAD_NAMESPACE")
        self.env["TEST_BAD_DSP"] = "1"
        self.run_shell(body, ok=False)

    def test_stock_boundary_preserves_radio_off_and_rejects_unfinished_ownership(self):
        fixture = self.stock_verification_fixture()
        self.env["TEST_RADIO_OFF"] = "1"
        self.run_shell(RECOVERY + fixture + "\nverify_stock_recovery")
        self.assertNotIn("namespace 333", (self.d / "stock-events").read_text())
        for name in ("latency-remote-uncertain", "latency-runtime.active", "latency-provider.pid", "dsp-trial.active", "qhs-nvm/intent"):
            with self.subTest(marker=name):
                path = self.pending_record(name)
                self.run_shell(RECOVERY + fixture + "\nverify_stock_recovery", ok=False)
                self.assertTrue(path.exists())
                path.unlink()

    def test_recovery_waits_for_full_hal_and_finalization_before_clearing_state(self):
        fixture = """
stop_policy() { :; }
profile_stop_runtime() { :; }
qhs_pending() { return 1; }
quiesce_radio() { :; }
host_bt() { :; }
restore_properties() { :; }
getprop() { echo 222; }
profile_recover_experiments() { :; }
setprop() { :; }
sleep() { :; }
check_inputs() { :; }
resume_radio() { touch "$MODDIR/radio-resumed"; }
profile_finish_runtime_recovery() { touch "$MODDIR/finish-attempted";return 43; }
"""
        for ready in [False, True]:
            with self.subTest(hal_ready=ready):
                (self.state / "active").touch()
                (self.state / "armed").write_text("pending\n")
                self.run_shell(RECOVERY + fixture +
                    ("profile_hal_ready() { :; }\n" if ready else "profile_hal_ready() { return 1; }\n") +
                    "lock_lifecycle\nrollback", ok=False)
                self.assertTrue((self.state / "active").exists())
                self.assertTrue((self.state / "armed").exists())
                self.assertEqual((self.d / "finish-attempted").exists(), ready)
                self.assertFalse((self.d / "radio-resumed").exists())

    def test_radio_resume_timeout_is_not_reported_as_success(self):
        resume = CONTROL[CONTROL.index("resume_radio()") : CONTROL.index("start_health()")]
        (self.state / "radio-on").write_text("1\n")
        fixture = """
cmd() { touch "$MODDIR/radio-attempted"; }
dumpsys() { echo '  State: OFF'; }
sleep() { :; }
"""
        self.run_shell(resume + fixture + "resume_radio", ok=False)
        self.assertTrue((self.d / "radio-attempted").exists())
        (self.d / "radio-attempted").unlink()
        (self.state / "radio-on").write_text("0\n")
        self.run_shell(resume + fixture + "resume_radio")
        self.assertFalse((self.d / "radio-attempted").exists())

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

    def test_action_opens_settings_without_mutating_audio_or_recovery(self):
        entry = self.recovery_entry_fixture("action.sh")
        (self.d / "settings.sh").write_text('echo settings-opened\n')
        journal = self.pending_record("mounts.tsv")
        original = journal.read_bytes()
        result = subprocess.run(SHELL + [str(entry)], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("settings-opened", result.stdout)
        self.assertEqual(journal.read_bytes(), original)
        self.assertFalse((self.d / "calls").exists())

    def test_apply_restores_orphaned_state_before_starting_another_trial(self):
        entry = self.recovery_entry_fixture("apply.sh")
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

    def test_recovery_preserves_state_when_policy_cannot_be_stopped(self):
        self.recovery_fixture()
        self.run_shell(
            RECOVERY + "\nstop_policy() { return 29; }\nlock_lifecycle\nrollback trial new",
            ok=False,
        )
        self.assertTrue((self.state / "armed").exists())
        self.assertTrue((self.state / "health-token").exists())
        self.assertTrue((self.state / "qhs-nvm/intent").exists())
        self.assertFalse((self.d / "recovery-attempted").exists())

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
        policy = next(
            line
            for line in (ROOT / "module/lib/policy.sh").read_text().splitlines()
            if "setsid" in line and '/config.sh" watch' in line
        )
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

    def test_keep_prepares_health_and_policy_before_disarming(self):
        helper = CONTROL[CONTROL.index("keep_session()") : CONTROL.index("allowed()")]
        for failing in ["health", "policy", "none"]:
            with self.subTest(failing=failing):
                (self.state / "armed").write_text("current\n")
                script = (
                    helper
                    + f"""
verify_active_payloads() {{ :; }}
start_health() {{ [ -f "$STATE/armed" ] && [ {failing} != health ]; }}
start_policy() {{ [ -f "$STATE/armed" ] && [ {failing} != policy ]; }}
keep_session
"""
                )
                self.run_shell(script, ok=failing == "none")
                self.assertEqual((self.state / "armed").exists(), failing != "none")

    def test_policy_config_and_worker_failures_are_propagated(self):
        helper = CONTROL[CONTROL.index("start_policy()") : CONTROL.index("keep_session()")]
        (self.state / "active").touch()
        failure = 'stop_policy() { touch "$MODDIR/policy-called"; return 23; }\nrequire_qhs_backend() { :; }\n'
        for enabled in ["true", "false"]:
            self.run_shell(
                helper + failure + f"\nextension_value() {{ echo {enabled}; }}\nstart_policy",
                ok=False,
            )
            self.assertTrue((self.d / "policy-called").exists())
            (self.d / "policy-called").unlink()
        self.run_shell(helper + "\nextension_value() { return 24; }\nstart_policy", ok=False)
        self.assertFalse((self.d / "policy-called").exists())

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
        (self.state / "experiment-unverified").touch()
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
        self.assertTrue((self.state / "experiment-unverified").exists())

    def test_stale_dsp_request_is_retired_without_device_operations(self):
        request = self.state / "dsp-trial.request"
        request.write_text("previous-boot deadbeef /untrusted/path\n")
        request.chmod(0o600)
        self.run_shell(
            'nsrun() { touch "$MODDIR/mutation"; return 1; }\n'
            'setprop() { touch "$MODDIR/mutation"; return 1; }\nprofile_activate_experiments'
        )
        self.assertFalse(request.exists())
        self.assertTrue(request.with_suffix(".request.stale").exists())
        self.assertFalse((self.d / "mutation").exists())

    def test_bad_dsp_candidate_is_rejected_before_device_operations(self):
        boot = pathlib.Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        request = self.state / "dsp-trial.request"
        request.write_text(boot + " deadbeef /shared-storage/candidate.so\n")
        request.chmod(0o600)
        self.run_shell(
            'nsrun() { touch "$MODDIR/mutation"; return 1; }\n'
            'setprop() { touch "$MODDIR/mutation"; return 1; }\nprofile_activate_experiments',
            ok=False,
        )
        self.assertTrue(request.exists())
        self.assertFalse((self.d / "mutation").exists())

    def test_dsp_recovery_keeps_marker_until_stock_and_restart_succeed(self):
        marker = self.state / "dsp-trial.active"
        marker.write_text("owned trial\n")
        self.run_shell(
            "nsrun() { echo unknown; }\n"
            'setprop() { touch "$MODDIR/mutation"; return 1; }\nprofile_recover_experiments',
            ok=False,
        )
        self.assertTrue(marker.exists())
        self.assertFalse((self.d / "mutation").exists())
        stock = "05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3"
        self.run_shell(
            "nsrun() { echo " + stock + "; }\ngetprop() { echo 1; }\n"
            "setprop() { return 1; }\nprofile_recover_experiments",
            ok=False,
        )
        self.assertTrue(marker.exists())

    def test_current_boot_dsp_request_is_blocked_before_any_mutation(self):
        request = self.state / "dsp-trial.request"
        request.write_text("current-boot cdb67b33ae62e67e4d3cb160870c2c24eaa860fb750426ae8b2e41ca872f0646 /unused\n")
        request.chmod(0o600)
        result = self.run_shell(
            'cat() { if [ "$1" = /proc/sys/kernel/random/boot_id ];then echo current-boot;else command cat "$@";fi; }\n'
            'setprop() { touch "$MODDIR/mutation"; }\nprofile_activate_experiments', ok=False)
        self.assertIn("signature verification", result.stderr)
        self.assertTrue(request.exists())
        self.assertFalse((self.d / "mutation").exists())
        availability = json.loads(self.run_shell("profile_tuning_availability").stdout)
        self.assertFalse(availability["runtime_apply_supported"])

    def test_pending_module_removal_cleans_internal_ui_in_current_root_session(self):
        (self.d / "lib").mkdir()
        (self.d / "lib/runtime.sh").write_text((ROOT / "module/lib/runtime.sh").read_text() + "\nensure_busybox_shell() { :; }\n")
        owner = self.d / "ui-lifecycle.sh"
        owner.write_text((ROOT / "module/ui-lifecycle.sh").read_text().replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"'))
        (self.d / "remove").touch()
        for script in ("control.sh", "settings-bridge.sh"):
            (self.d / script).write_text('printf \'%s %s\\n\' "${0##*/}" "$1" >> "${0%/*}/cleanup-calls"\n')
        pm = self.d / "pm"
        pm.write_text('#!/bin/sh\nprintf \'pm %s\\n\' "$*" >> '+shlex.quote(str(self.d / "cleanup-calls"))+'\n'
            + 'if [ "$1" = list ];then [ -f '+shlex.quote(str(self.d / "ui-removed"))+' ] || echo package:dev.shizuka.settings;fi\n'
            + 'if [ "$1" = uninstall ];then touch '+shlex.quote(str(self.d / "ui-removed"))+';fi\n')
        pm.chmod(0o755)
        run = subprocess.run(SHELL + [str(owner), "watch"], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = (self.d / "cleanup-calls").read_text().splitlines()
        self.assertEqual(calls, ["control.sh rollback", "settings-bridge.sh stop", "pm list packages dev.shizuka.settings", "pm uninstall dev.shizuka.settings", "control.sh rollback", "settings-bridge.sh stop", "pm list packages dev.shizuka.settings"])

    def test_uninstaller_removes_internal_ui_even_when_audio_recovery_reports_failure(self):
        uninstall = self.d / "uninstall.sh"
        uninstall.write_text((ROOT / "module/uninstall.sh").read_text().replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"'))
        (self.d / "control.sh").write_text("exit 3\n")
        pm = self.d / "pm"
        pm.write_text('#!/bin/sh\nprintf \'%s %s\\n\' "$1" "$2" >> '+shlex.quote(str(self.d / "pm-calls"))+'\n')
        pm.chmod(0o755)
        run = subprocess.run(SHELL + [str(uninstall)], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(run.returncode, 3, run.stderr)
        self.assertEqual((self.d / "pm-calls").read_text().splitlines(), ["path dev.shizuka.settings", "uninstall dev.shizuka.settings"])

    def test_all_module_scripts_have_valid_shell_syntax(self):
        for script in (ROOT / "module").rglob("*.sh"):
            result = subprocess.run(SHELL + ["-n", str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, str(script) + result.stderr)


if __name__ == "__main__":
    unittest.main()
