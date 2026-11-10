# SPDX-License-Identifier: GPL-2.0-only
"""Run actual latency lifecycle shell code against private fake Android boundaries."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))
STOCK = "4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc"
BOOT = "00112233-4455-6677-8899-aabbccddeeff"
COOKIE = "0123456789abcdef0123456789abcdef"


class LatencyLifecycle(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-latency-test-")
        self.addCleanup(self.temporary.cleanup)
        self.d = Path(self.temporary.name)
        self.state = self.d / "state"
        self.state.mkdir(mode=0o700)
        (self.d / "lib").mkdir()
        (self.d / "payload").mkdir()
        self.public = self.d / "public"
        self.target = self.d / "stock-gsl.so"
        self.candidate = self.d / "payload/patched-libar-gsl.so"
        self.candidate.write_bytes(b"synthetic GSL candidate; not an ELF or audio payload")
        self.digest = hashlib.sha256(self.candidate.read_bytes()).hexdigest()
        (self.d / "boot").write_text(BOOT + "\n")
        (self.d / "live-hash").write_text(STOCK)
        (self.d / "mountinfo").write_text("")
        (self.d / "hal.maps").write_text("0000-1000 r-xp /vendor/lib64/libar-gsl.so\n")
        runtime = (ROOT / "module/lib/runtime.sh").read_text().replace("process_start() (", "host_process_start() (")
        runtime += r'''
ensure_busybox_shell() { :; }
cat() {
 if [ "$1" = /proc/sys/kernel/random/boot_id ];then command cat "$MODDIR/boot";else command cat "$@";fi
}
process_start() {
 if [ "$TEST_REAL_PROC" = 1 ];then host_process_start "$@";return;fi
 if [ "$1" = 4242 ];then
  [ -f "$STATE/alive" ] || return 1
  command cat "$STATE/current-start"
 else
  [ "$TEST_STARTUP_EXIT" = 0 ] || return 1
  echo 1234
 fi
}
kill() {
 printf '%s\n' "$*" >> "$STATE/signals"
 if [ "$TEST_REAL_PROC" = 1 ];then command kill "$@";return;fi
 if [ "$TEST_NEVER_DIES" = 1 ];then return 0;fi
 if [ "$1" = -TERM ] && [ "$TEST_IGNORE_TERM" = 1 ];then return 0;fi
 rm -f "$STATE/alive"
}
sleep() { :; }
'''
        (self.d / "lib/runtime.sh").write_text(runtime)
        source = (ROOT / "module/latency-runtime.sh").read_text()
        source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        source = source.replace("TARGET=/vendor/lib64/libar-gsl.so", 'TARGET="$MODDIR/stock-gsl.so"')
        source = source.replace("directory=/data/vendor/audio/shizuka", 'directory="$MODDIR/public"')
        source = source.replace("/proc/1/mountinfo", '"$MODDIR/mountinfo"')
        source = source.replace('"/proc/$pid/cmdline"', '"$STATE/provider.cmdline"')
        source = source.replace('"/proc/$2/maps"', '"$MODDIR/hal.maps"')
        self.entry = self.d / "latency-runtime.sh"
        self.entry.write_text(source)
        self.bb = self.d / "bb"
        self.bb.write_text(f"#!{sys.executable}\n" + r'''
import fcntl,hashlib,json,os,pathlib,stat,sys
d=pathlib.Path(os.environ["TEST_ROOT"]); a=sys.argv[1:]
def log(**extra):
 with (d/"events").open("a") as out:out.write(json.dumps({"args":a,**extra})+"\n")
if a[0]=="stat":
 p=pathlib.Path(a[3]);s=p.stat();fmt=a[2]
 if fmt=="%u":print(os.environ.get("TEST_OWNER","0"))
 elif fmt=="%u:%a":print(os.environ.get("TEST_OWNER","0")+":"+format(stat.S_IMODE(s.st_mode),"o"))
 elif fmt=="%u:%g:%a:%h":
  print(os.environ.get("TEST_STATUS_UID","1041")+":1005:"+format(stat.S_IMODE(s.st_mode),"o")+":"+str(s.st_nlink))
 else:raise SystemExit("unsupported stat fixture: "+fmt)
elif a[0]=="flock":fcntl.flock(int(a[1]),fcntl.LOCK_EX)
elif a[0]=="readlink":print("mnt:[99]")
elif a[0]=="sha256sum":print(hashlib.sha256(pathlib.Path(a[1]).read_bytes()).hexdigest()+"  "+a[1])
elif a[0]=="nsenter":
 inner=a[a.index("--")+1:]
 if inner[0]==str(d/"bb"):inner=inner[1:]
 if inner[0]=="sha256sum":
  dependencies={"/vendor/lib64/libar-gpr.so":"1339b2bcda1415535ec53666ceb4f873c3a7d33faf2a3cb55af3702a0a306d15",
                "/vendor/lib64/liblx-osal.so":"1d81e09e7eb9b9442218127036acb255f56857cadd220ed85dbed532619586ef"}
  if os.environ.get("TEST_BAD_DEPENDENCY") in dependencies:dependencies[os.environ["TEST_BAD_DEPENDENCY"]]="f"*64
  print(dependencies.get(inner[1],(d/"live-hash").read_text())+"  "+inner[1])
 elif inner[0]=="mount":
  journal=(d/"state/mounts.tsv").read_text()
  owned=(d/"state/latency-runtime.active").is_file() and str(d/"stock-gsl.so") in journal
  prepared=(d/"public/status.bin").is_file() and (d/"public/status.bin").stat().st_size==256
  log(journaled_before_mount=owned,status_prepared_before_mount=prepared)
  if not owned or not prepared:raise SystemExit(91)
  if os.environ.get("TEST_MOUNT_FAIL")=="1":raise SystemExit(42)
  (d/"live-hash").write_text(hashlib.sha256(pathlib.Path(inner[-2]).read_bytes()).hexdigest())
 else:raise SystemExit("forbidden namespace fixture command")
elif a[0]=="setsid":
 log();(d/"provider-launch").write_text(json.dumps(a[1:]))
elif a[0]=="dd":
 log();os.execvp(a[0],a)
else:os.execvp(a[0],a)
''')
        self.bb.chmod(0o755)
        for name in ("chown", "chcon", "restorecon"):
            path = self.d / name
            path.write_text(f"#!{sys.executable}\nimport os,pathlib,sys\n"
                            "with (pathlib.Path(os.environ['TEST_ROOT'])/'boundary-calls').open('a') as out:\n"
                            " out.write(pathlib.Path(sys.argv[0]).name+' '+' '.join(sys.argv[1:])+'\\n')\n")
            path.chmod(0o755)
        (self.d / "id").write_text("#!/bin/sh\ncase \"$*\" in '-u audioserver') echo 1041;;'-g audio') echo 1005;;*) echo 0;;esac\n")
        (self.d / "id").chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.d) + os.pathsep + os.environ["PATH"],
                        TEST_ROOT=str(self.d), TEST_REAL_PROC="0", TEST_STARTUP_EXIT="0",
                        TEST_NEVER_DIES="0", TEST_IGNORE_TERM="0")

    def run_entry(self, *arguments, ok=True, extra=None):
        result = subprocess.run(SHELL + [str(self.entry), *map(str, arguments)],
                                env=dict(self.env, **(extra or {})), text=True, capture_output=True, timeout=6)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def record(self, name="latency-runtime.request", *, boot=BOOT, cookie=COOKIE, digest=None, mode="observer", extra=""):
        path = self.state / name
        path.write_text(f"{boot} {cookie} {digest or self.digest} {mode}{extra}\n")
        path.chmod(0o600)
        return path

    def events(self):
        path = self.d / "events"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def no_mount(self):
        self.assertFalse(any(e["args"][0] == "nsenter" and "mount" in e["args"] for e in self.events()))

    def own_provider(self, *, current_start="1234"):
        (self.state / "latency-provider.pid").write_text("4242\n")
        (self.state / "latency-provider.start").write_text("1234\n")
        (self.state / "current-start").write_text(current_start + "\n")
        (self.state / "alive").touch()
        (self.state / "provider.cmdline").write_bytes(str(self.d / "latency-provider").encode() + b"\0--cookie\0" + COOKIE.encode())

    def test_absent_request_is_noop(self):
        self.run_entry("mount")
        self.assertEqual(self.events(), [])
        self.assertFalse((self.state / "latency-runtime.active").exists())

    def test_earlier_boot_request_is_archived_without_mount(self):
        previous = "11223344-5566-7788-99aa-bbccddeeff00"
        request = self.record(boot=previous)
        self.run_entry("mount")
        self.assertFalse(request.exists())
        self.assertTrue((self.state / f"latency-runtime.request.stale.{previous}.{COOKIE}").exists())
        self.no_mount()

    def test_stale_mount_request_preserves_colliding_archive_and_rejects_invalid_boot(self):
        previous = "11223344-5566-7788-99aa-bbccddeeff00"
        request = self.record(boot=previous)
        before = request.read_bytes()
        archive = self.state / f"latency-runtime.request.stale.{previous}.{COOKIE}"
        archive.write_bytes(b"prior recovery evidence")
        self.run_entry("mount", ok=False)
        self.assertEqual(archive.read_bytes(), b"prior recovery evidence")
        self.assertEqual(request.read_bytes(), before)
        self.record(boot="invalid-prior-boot")
        self.run_entry("mount", ok=False)
        self.assertTrue(request.exists())
        self.no_mount()

    def test_invalid_identity_mode_and_extra_fields_fail_before_mount(self):
        for options in ({"cookie": "0"*32}, {"cookie": "F"*32}, {"cookie": "a"},
                        {"digest": "b"}, {"mode": "unknown"}, {"extra": " extra"}):
            with self.subTest(options=options):
                self.record(**options)
                self.run_entry("mount", ok=False)
                self.no_mount()

    def test_private_record_mode_and_owner_are_required(self):
        path = self.record()
        path.chmod(0o644)
        self.run_entry("mount", ok=False)
        path.chmod(0o600)
        self.run_entry("mount", ok=False, extra={"TEST_OWNER": "1000"})
        self.no_mount()

    def test_disabled_and_removed_modules_cannot_mount(self):
        self.record()
        for marker in ("disable", "remove"):
            (self.d / marker).touch()
            self.run_entry("mount", ok=False)
            (self.d / marker).unlink()
        self.no_mount()

    def test_hash_or_existing_mount_mismatch_refuses_overlay(self):
        self.record(digest="f"*64)
        self.run_entry("mount", ok=False)
        self.record()
        (self.d / "live-hash").write_text("e"*64)
        self.run_entry("mount", ok=False)
        (self.d / "live-hash").write_text(STOCK)
        (self.d / "mountinfo").write_text("1 2 3 / "+str(self.target)+" options\n")
        self.run_entry("mount", ok=False)
        self.no_mount()

    def test_unresolved_active_record_or_symlink_is_not_overwritten(self):
        self.record()
        active = self.state / "latency-runtime.active"
        active.write_text("prior generation")
        self.run_entry("mount", ok=False)
        self.assertEqual(active.read_text(), "prior generation")
        active.unlink()
        target = self.d / "keep"
        target.write_text("keep")
        active.symlink_to(target)
        self.run_entry("mount", ok=False)
        self.assertEqual(target.read_text(), "keep")
        self.no_mount()

    def test_unknown_gpr_or_allocator_is_rejected_before_mount(self):
        self.record()
        for dependency in ("/vendor/lib64/libar-gpr.so", "/vendor/lib64/liblx-osal.so"):
            with self.subTest(dependency=dependency):
                self.run_entry("mount",ok=False,extra={"TEST_BAD_DEPENDENCY":dependency})
                self.assertFalse((self.state/"latency-runtime.active").exists())
                self.no_mount()

    def test_mount_prepares_status_and_ownership_before_mutation(self):
        request = self.record()
        self.run_entry("mount")
        active = self.state / "latency-runtime.active"
        self.assertEqual(active.read_bytes(), request.read_bytes())
        self.assertEqual(active.stat().st_mode & 0o777, 0o600)
        self.assertTrue((self.state / "experiment-unverified").is_file())
        self.assertEqual((self.public / "status.bin").stat().st_size, 256)
        self.assertEqual((self.public / "status.bin").stat().st_mode & 0o777, 0o640)
        event = next(e for e in self.events() if "journaled_before_mount" in e)
        self.assertTrue(event["journaled_before_mount"])
        self.assertTrue(event["status_prepared_before_mount"])
        self.assertFalse(list(self.state.glob(".latency-runtime.*")))

    def test_failed_mount_retains_recovery_evidence(self):
        self.record()
        self.run_entry("mount", ok=False, extra={"TEST_MOUNT_FAIL": "1"})
        self.assertTrue((self.state / "latency-runtime.active").exists())
        self.assertIn(str(self.target), (self.state / "mounts.tsv").read_text())

    def test_status_hardlink_symlink_and_wrong_owner_are_not_truncated(self):
        for kind in ("hardlink", "symlink", "owner", "fifo"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as outside:
                self.record()
                self.public.mkdir(exist_ok=True)
                status = self.public / "status.bin"
                keep = Path(outside) / "keep"
                keep.write_bytes(b"K"*256)
                keep.chmod(0o640)
                if kind == "hardlink":
                    os.link(keep, status)
                elif kind == "symlink":
                    status.symlink_to(keep)
                elif kind == "fifo":
                    os.mkfifo(status, 0o640)
                else:
                    status.write_bytes(b"K"*256)
                    status.chmod(0o640)
                self.run_entry("mount", ok=False, extra={"TEST_STATUS_UID": "1000"} if kind == "owner" else None)
                self.assertEqual(keep.read_bytes(), b"K"*256)
                if kind == "owner":
                    self.assertEqual(status.read_bytes(), b"K"*256)
                self.no_mount()
                status.unlink()
                (self.state / "latency-runtime.active").unlink()

    def test_start_requires_current_activation_module_and_publisher_hash(self):
        self.run_entry("start")
        self.record("latency-runtime.active", boot="earlier-boot")
        self.run_entry("start", ok=False)
        self.record("latency-runtime.active")
        self.run_entry("start", ok=False)
        (self.state / "active").touch()
        (self.d / "latency-provider").write_text("synthetic publisher")
        (self.d / "latency-provider.sha256").write_text("f"*64)
        self.run_entry("start", ok=False)
        self.assertFalse((self.d / "provider-launch").exists())

    def test_start_passes_bound_identity_without_resetting_hal_status(self):
        self.record("latency-runtime.active")
        (self.state / "active").touch()
        (self.state / "hal.pid").write_text("7777\n")
        provider = self.d / "latency-provider"
        provider.write_text("synthetic publisher")
        (self.d / "latency-provider.sha256").write_text(hashlib.sha256(provider.read_bytes()).hexdigest()+"\n")
        self.public.mkdir()
        status = self.public / "status.bin"
        status.write_bytes(b"S"*256)
        self.run_entry("start")
        for _ in range(50):
            if (self.d / "provider-launch").exists():
                break
            time.sleep(0.01)
        launch = json.loads((self.d / "provider-launch").read_text())
        self.assertIn(COOKIE, launch)
        self.assertEqual(launch[-2:], ["--hal-pid", "7777"])
        self.assertEqual(status.read_bytes(), b"S"*256)

    def health_provider(self):
        path = self.d / "latency-provider"
        path.write_text(f"#!{sys.executable}\n" + r'''
import json,os,pathlib,sys
d=pathlib.Path(os.environ["TEST_ROOT"])
(d/"health-provider-call").write_text(json.dumps(sys.argv[1:]))
print('{"healthy":'+('true' if os.environ.get('TEST_HEALTH_EXIT','0')=='0' else 'false')+'}')
raise SystemExit(int(os.environ.get('TEST_HEALTH_EXIT','0')))
''')
        path.chmod(0o755)
        (self.d / "latency-provider.sha256").write_text(hashlib.sha256(path.read_bytes()).hexdigest()+"\n")

    def test_health_inactive_is_noop_and_active_uses_fixed_readonly_arguments(self):
        self.run_entry("health")
        self.assertFalse((self.d / "health-provider-call").exists())
        self.record("latency-runtime.active")
        self.run_entry("health", ok=False)
        (self.state / "active").touch()
        self.health_provider()
        self.own_provider()
        self.run_entry("health")
        self.assertEqual(json.loads((self.d / "health-provider-call").read_text()),
                         ["--health", "--module-dir", str(self.d)])
        self.assertFalse((self.state / "signals").exists())

    def test_health_failure_keeps_ownership_for_normal_root_recovery(self):
        self.record("latency-runtime.active")
        (self.state / "active").touch()
        self.health_provider()
        self.own_provider()
        result = self.run_entry("health", ok=False, extra={"TEST_HEALTH_EXIT": "2"})
        self.assertEqual(result.returncode, 2)
        self.assertTrue((self.state / "latency-runtime.active").exists())
        self.assertTrue((self.state / "latency-provider.pid").exists())
        self.assertFalse((self.state / "signals").exists())

    def test_health_rejects_stale_or_replaced_process_and_unverified_binary(self):
        self.record("latency-runtime.active")
        (self.state / "active").touch()
        self.health_provider()
        self.own_provider(current_start="9999")
        self.run_entry("health", ok=False)
        self.assertFalse((self.d / "health-provider-call").exists())
        self.own_provider()
        (self.d / "latency-provider.sha256").write_text("f"*64)
        self.run_entry("health", ok=False)
        self.assertFalse((self.d / "health-provider-call").exists())
        self.health_provider()
        (self.d / "disable").touch()
        self.run_entry("health", ok=False)
        self.assertFalse((self.d / "health-provider-call").exists())

    def test_health_broken_active_symlink_is_rejected_instead_of_inactive(self):
        (self.state / "latency-runtime.active").symlink_to(self.d / "absent")
        self.run_entry("health", ok=False)
        self.assertFalse((self.d / "health-provider-call").exists())

    def test_term_cleanup_removes_only_owned_process_records(self):
        self.own_provider()
        self.run_entry("stop")
        self.assertEqual((self.state / "signals").read_text().splitlines(), ["-TERM 4242"])
        self.assertFalse((self.state / "latency-provider.pid").exists())

    def test_term_timeout_escalates_and_confirms_kill(self):
        self.own_provider()
        self.run_entry("stop", extra={"TEST_IGNORE_TERM": "1"})
        self.assertEqual((self.state / "signals").read_text().splitlines(), ["-TERM 4242", "-KILL 4242"])
        self.assertFalse((self.state / "latency-provider.pid").exists())

    def test_unconfirmed_termination_preserves_ownership(self):
        self.own_provider()
        result = self.run_entry("stop", ok=False, extra={"TEST_NEVER_DIES": "1"})
        self.assertIn("ownership records retained", result.stderr)
        self.assertEqual((self.state / "latency-provider.pid").read_text(), "4242\n")
        self.assertEqual((self.state / "latency-provider.start").read_text(), "1234\n")

    def test_reused_pid_or_unrelated_command_is_not_killed(self):
        self.own_provider(current_start="9999")
        self.run_entry("stop")
        self.assertFalse((self.state / "signals").exists())
        self.own_provider()
        (self.state / "provider.cmdline").write_bytes(b"unrelated-process\0")
        self.run_entry("stop")
        self.assertFalse((self.state / "signals").exists())

    def test_recovery_requires_stock_before_deleting_activation_record(self):
        active = self.record("latency-runtime.active")
        (self.d / "live-hash").write_text(self.digest)
        self.run_entry("recovered", ok=False)
        self.assertTrue(active.exists())
        (self.d / "live-hash").write_text(STOCK)
        self.run_entry("recovered")
        self.assertFalse(active.exists())

    def test_verify_binds_candidate_view_and_mapped_hal(self):
        self.record("latency-runtime.active")
        self.run_entry("verify", "7777", ok=False)
        (self.d / "live-hash").write_text(self.digest)
        self.run_entry("verify", "7777")
        (self.d / "hal.maps").write_text("")
        self.run_entry("verify", "7777", ok=False)

    def test_linux_real_process_ignoring_term_is_killed_and_reaped(self):
        if not Path("/proc/self/stat").exists():
            self.skipTest("requires Linux numeric process identities")
        child = subprocess.Popen([sys.executable, "-c",
            "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);print('ready',flush=True);time.sleep(30)"],
            stdout=subprocess.PIPE, text=True)
        def cleanup():
            if child.poll() is None:
                child.kill()
            child.wait(timeout=3)
            child.stdout.close()
        self.addCleanup(cleanup)
        self.assertEqual(child.stdout.readline().strip(), "ready")
        pid = child.pid
        start = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()[19]
        (self.state / "latency-provider.pid").write_text(str(pid)+"\n")
        (self.state / "latency-provider.start").write_text(start+"\n")
        (self.state / "provider.cmdline").write_bytes(str(self.d / "latency-provider").encode()+b"\0")
        self.run_entry("stop", extra={"TEST_REAL_PROC": "1"})
        self.assertEqual(child.wait(timeout=3), -9)
        self.assertFalse((self.state / "latency-provider.pid").exists())

    def test_normal_profile_hooks_do_not_require_experimental_script(self):
        profile = ROOT / "module/profiles/samsung-sm-s931z-dzif.sh"
        script = f'MODDIR={shlex.quote(str(self.d))};STATE="$MODDIR/state";BB="$MODDIR/bb"\n'
        script += f". {shlex.quote(str(profile))}\nprofile_start_runtime\nprofile_stop_runtime\nprofile_verify_runtime 7777\n"
        self.entry.unlink()
        result = subprocess.run(SHELL + ["-eu", "-c", script], env=self.env, text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_control_starts_publisher_only_after_active_marker(self):
        control = (ROOT / "module/control.sh").read_text()
        tail = '  record_process hal "$new_hal"' + control.split('  record_process hal "$new_hal"', 1)[1]
        tail = tail.split('\n  log "loaded experiment', 1)[0]
        body = f'MODDIR={shlex.quote(str(self.d))};STATE="$MODDIR/state";BB="$MODDIR/bb"\n'
        body += r'''
record_process() { echo "$*" >> "$STATE/control-calls"; }
nsrun() { :; }
profile_start_runtime() { [ -f "$STATE/active" ] || exit 91;echo runtime-start >> "$STATE/control-calls"; }
new_hal=7777;zpid=1;NATIVE=unused-native;APK=unused-apk;qhs_enabled=false
'''
        result = subprocess.run(SHELL + ["-eu", "-c", body + tail], env=self.env, text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.state / "control-calls").read_text().splitlines(), ["hal 7777", "runtime-start"])

    def test_control_stop_failure_retains_recovery_before_quiescing(self):
        control = (ROOT / "module/control.sh").read_text()
        recovery = control[control.index("qhs_features()"):control.index('case "'+chr(36)+'{1:-status}"')]
        body = f'MODDIR={shlex.quote(str(self.d))};STATE="$MODDIR/state";BB="$MODDIR/bb"\n'
        body += f'. {shlex.quote(str(self.d / "lib/runtime.sh"))}\n'
        body += r'''
fail() { echo "$*" >&2;exit 1; }
lock_lifecycle() { :; }
lock_policy() { :; }
stop_policy() { :; }
recovery_pending() { return 0; }
profile_stop_runtime() { echo stop >> "$STATE/control-calls";return 1; }
quiesce_radio() { touch "$STATE/quiesced"; }
'''
        (self.state / "armed").write_text("keep ownership")
        result = subprocess.run(SHELL + ["-eu", "-c", body + recovery + "\nrollback\n"],
                                env=self.env, text=True, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.state / "armed").exists())
        self.assertFalse((self.state / "quiesced").exists())
        self.assertEqual((self.state / "control-calls").read_text(), "stop\n")


if __name__ == "__main__":
    unittest.main()
