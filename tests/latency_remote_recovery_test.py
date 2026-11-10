# SPDX-License-Identifier: GPL-2.0-only
"""Run remote-ownership recovery against private Android/service boundaries."""

import hashlib
import importlib.util
import json
import os
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "latency_remote_request_fixture", ROOT / "tests/latency_request_test.py"
)
FIXTURES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURES)
BASE = FIXTURES.FIXTURES
OLD_BOOT = "11223344-5566-7788-99aa-bbccddeeff00"


class LatencyRemoteRecovery(unittest.TestCase):
    def setUp(self):
        # Reuse only setup/boundaries, without inheriting the unrelated tests.
        self.request_fixture = FIXTURES.LatencyRequest(methodName="runTest")
        self.addCleanup(self.request_fixture.doCleanups)
        self.request_fixture.setUp()
        self.f = self.request_fixture.f
        self.d, self.state = self.f.d, self.f.state
        for name in (
            "module.prop",
            "device-profile",
            "payload/patched-libar-gsl.so",
            "payload/patched-libbluetooth_jni.so",
            "payload/patched-libsehbluetooth_audio_session_aidl.so",
        ):
            (self.d / name).chmod(0o644)
        bb = self.f.bb.read_text().replace(
            'elif a[0]=="flock":',
            'elif a[0]=="sync":\n log();raise SystemExit(int(os.environ.get("TEST_SYNC_FAIL","0")))\n'
            'elif a[0]=="flock":',
        )
        self.f.bb.write_text(bb)
        self.journal = self.state / "latency-remote-uncertain"
        self.f.record("latency-runtime.active", mode="relay")
        (self.state / "hal.pid").write_text("7777\n")
        (self.state / "hal.start").write_text("2222\n")
        (self.state / "active").touch()
        self.f.own_provider()
        self.provider = self.d / "latency-provider"
        self.provider.write_text(
            "#!"
            + BASE.sys.executable
            + "\n"
            + r"""
import json,os,pathlib,sys
d=pathlib.Path(os.environ["TEST_ROOT"])
with (d/"recovery-provider-calls").open("a") as out:out.write(json.dumps(sys.argv[1:])+"\n")
if os.environ.get("TEST_RECOVERY_MALFORMED")=="1":print("not a status receipt")
else:print(json.dumps({"schema":1,"safe_without_dsp_restart":os.environ.get("TEST_RECOVERY_SAFE")=="1"},separators=(",",":")))
raise SystemExit(int(os.environ.get("TEST_RECOVERY_EXIT","2")))
"""
        )
        self.provider.chmod(0o755)
        provider_hash = self.d / "latency-provider.sha256"
        provider_hash.write_text(hashlib.sha256(self.provider.read_bytes()).hexdigest() + "\n")
        provider_hash.chmod(0o644)
        # Capture the actual journal at the instant the existing TERM boundary
        # executes. A later journal write cannot satisfy this ordering check.
        signal_observer = self.d / "signal-observer"
        signal_observer.write_text(
            "#!"
            + BASE.sys.executable
            + "\n"
            + r"""
import json,os,pathlib,stat,sys
d=pathlib.Path(os.environ["TEST_ROOT"]);p=d/"state/latency-remote-uncertain"
entry={"signal":sys.argv[1:],"journal_exists":p.is_file() and not p.is_symlink()}
if entry["journal_exists"]:entry.update(journal=p.read_text(),mode=stat.S_IMODE(p.stat().st_mode),links=p.stat().st_nlink)
with (d/"signal-journal").open("a") as out:out.write(json.dumps(entry)+"\n")
"""
        )
        signal_observer.chmod(0o755)
        runtime = self.d / "lib/runtime.sh"
        text = runtime.read_text().replace(
            "kill() {\n", 'kill() {\n "$MODDIR/signal-observer" "$@"\n'
        )
        runtime.write_text(text)
        # Device-specific hooks are the Android/normal-service boundary here.
        # Journal parsing, transitions, ordering and deletion remain real code.
        (self.d / "lib/device.sh").write_text(r"""
profile_latency_recovery_preflight() {
 printf 'preflight\n' >> "$MODDIR/recovery-boundaries"
 [ "${TEST_PREFLIGHT_FAIL:-0}" = 0 ]
}
profile_latency_hal_identity() { printf '%s\n' "${TEST_STOP_HAL:-7777 2222}"; }
profile_latency_stop_hal() {
 printf 'stop-hal %s %s\n' "$1" "$2" >> "$MODDIR/recovery-boundaries"
 [ "${TEST_STOP_FAIL:-0}" = 0 ]
}
profile_latency_restart_dsp() {
 printf 'restart-dsp\n' >> "$MODDIR/recovery-boundaries"
 cat "$STATE/latency-remote-uncertain" > "$MODDIR/journal-at-ssr"
 [ "${TEST_SSR_FAIL:-0}" = 0 ] || return 1
 : > "$MODDIR/ssr-written"
}
profile_latency_dsp_state() {
 count=0;[ ! -f "$MODDIR/state-reads" ] || count=$(cat "$MODDIR/state-reads")
 count=$((count+1));printf '%s\n' "$count" > "$MODDIR/state-reads"
 case "${TEST_DSP_STATES:-offline-running}" in
  running-only) echo running;;offline-only) echo offline;;
  offline-running) if [ "$count" -le 1 ];then echo offline;else echo running;fi;;
  *) echo crashed;;
 esac
}
profile_latency_normal_ready() {
 printf 'normal-ready %s %s\n' "$1" "$2" >> "$MODDIR/recovery-boundaries"
 [ "${TEST_NORMAL_READY:-1}" = 1 ] && [ "$1" = 8888 ] && [ "$1" != "$2" ]
}
""")

    def run_entry(self, *args, ok=True, extra=None):
        return self.f.run_entry(*args, ok=ok, extra=extra)

    def journal_fields(self):
        fields = self.journal.read_text().split()
        self.assertEqual(len(fields), 10)
        return fields

    def write_journal(
        self,
        *,
        phase="pending",
        boot=BASE.BOOT,
        old_pid="7777",
        old_start="2222",
        stop_pid="0",
        stop_start="0",
        reason="status_unknown",
    ):
        # A manually staged recovery journal represents stop() having already
        # retired the publisher. Keep its old HAL identity independently.
        for name in ("latency-provider.pid", "latency-provider.start", "alive"):
            p = self.state / name
            if p.exists():
                p.unlink()
        self.journal.write_text(
            f"1 {boot} {BASE.COOKIE} {self.f.digest} {old_pid} {old_start} "
            f"{stop_pid} {stop_start} {phase} {reason}\n"
        )
        self.journal.chmod(0o600)

    def boundaries(self):
        p = self.d / "recovery-boundaries"
        return p.read_text().splitlines() if p.exists() else []

    def provider_calls(self):
        p = self.d / "recovery-provider-calls"
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def test_unknown_remote_state_is_journaled_before_provider_term(self):
        self.run_entry("stop")
        fields = self.journal_fields()
        self.assertEqual(fields[:6], ["1", BASE.BOOT, BASE.COOKIE, self.f.digest, "7777", "2222"])
        self.assertEqual(fields[8], "pending")
        self.assertIn(fields[9], ("status_unknown", "remote_ownership"))
        signals = [
            json.loads(line) for line in (self.d / "signal-journal").read_text().splitlines()
        ]
        self.assertEqual(signals[0]["signal"], ["-TERM", "4242"])
        self.assertTrue(signals[0]["journal_exists"])
        self.assertEqual(signals[0]["journal"], self.journal.read_text())
        self.assertEqual((signals[0]["mode"], signals[0]["links"]), (0o600, 1))
        self.assertEqual(self.boundaries(), [])
        self.assertFalse((self.state / "latency-provider.pid").exists())

    def test_verified_clean_closed_receipt_is_the_only_restart_exemption(self):
        self.run_entry("stop", extra={"TEST_RECOVERY_SAFE": "1", "TEST_RECOVERY_EXIT": "0"})
        self.assertEqual(self.journal_fields()[8], "pending")
        self.assertEqual(self.boundaries(), [])
        self.assertEqual(self.provider_calls(), [])
        self.run_entry("recovered", "8888", ok=False)
        self.assertTrue(self.journal.exists())
        self.run_entry("recover-dsp", extra={"TEST_RECOVERY_SAFE": "1", "TEST_RECOVERY_EXIT": "0"})
        self.assertEqual(self.journal_fields()[8], "clean")
        self.assertEqual(
            self.provider_calls(),
            [
                [
                    "--recovery-check",
                    "--module-dir",
                    str(self.d),
                    "--cookie",
                    BASE.COOKIE,
                    "--hal-pid",
                    "7777",
                    "--hal-start",
                    "2222",
                ]
            ],
        )
        self.assertEqual(self.boundaries(), [])
        self.assertTrue((self.state / "latency-runtime.active").exists())
        self.run_entry("recovered", "7777", ok=False)
        self.assertTrue(self.journal.exists())
        self.run_entry("recovered", "8888")
        self.assertFalse(self.journal.exists())
        self.assertFalse((self.state / "latency-runtime.active").exists())

    def test_late_closed_receipt_marks_clean_without_early_record_deletion(self):
        self.run_entry("stop", extra={"TEST_RECOVERY_SAFE": "0", "TEST_RECOVERY_EXIT": "2"})
        self.assertEqual(self.journal_fields()[8], "pending")
        self.assertEqual(self.provider_calls(), [])
        self.run_entry("recover-dsp", extra={"TEST_RECOVERY_SAFE": "1", "TEST_RECOVERY_EXIT": "0"})
        self.assertEqual(self.journal_fields()[8], "clean")
        self.assertTrue((self.state / "latency-runtime.active").exists())
        self.assertEqual(self.boundaries(), [])

    def test_exit_failure_or_malformed_receipt_cannot_exempt_restart(self):
        for extra in (
            {"TEST_RECOVERY_SAFE": "1"},
            {"TEST_RECOVERY_EXIT": "0", "TEST_RECOVERY_MALFORMED": "1"},
            {"TEST_RECOVERY_EXIT": "0", "TEST_RECOVERY_SAFE": "0"},
        ):
            with self.subTest(extra=extra):
                self.f.own_provider()
                if self.journal.exists():
                    self.journal.unlink()
                self.run_entry("stop", extra=extra)
                self.assertEqual(self.journal_fields()[8], "pending")
                self.run_entry("recover-dsp", extra=extra)
                self.assertEqual(self.journal_fields()[8], "ssr_ready")
                self.assertEqual(len(self.provider_calls()), 20)
                (self.d / "recovery-provider-calls").unlink()
                (self.d / "state-reads").unlink()

    def test_missing_verifier_conservatively_preserves_remote_uncertainty(self):
        self.provider.unlink()
        self.run_entry("stop")
        self.assertTrue(self.journal.is_file())
        self.assertEqual(self.boundaries(), [])
        self.run_entry("recover-dsp")
        self.assertEqual(self.journal_fields()[8], "ssr_ready")

    def test_provisional_clean_is_rechecked_and_demoted_when_raw_proof_is_lost(self):
        self.write_journal(phase="clean")
        self.run_entry("recover-dsp", extra={"TEST_RECOVERY_SAFE": "0", "TEST_RECOVERY_EXIT": "2"})
        self.assertEqual(self.journal_fields()[8], "ssr_ready")
        self.assertEqual(len(self.provider_calls()), 20)
        self.assertEqual(self.boundaries().count("restart-dsp"), 1)
        self.assertEqual((self.d / "journal-at-ssr").read_text().split()[8], "ssr_requested")

    def test_failed_journal_sync_never_terminates_the_publisher(self):
        self.run_entry("stop", ok=False, extra={"TEST_SYNC_FAIL": "1"})
        self.assertTrue(self.journal.is_file())
        self.assertTrue((self.state / "latency-provider.pid").exists())
        self.assertTrue((self.state / "alive").exists())
        self.assertFalse((self.d / "signal-journal").exists())

    def test_nonroot_or_nonprivate_state_refuses_before_publisher_term(self):
        self.run_entry("stop", ok=False, extra={"TEST_UID": "1000"})
        self.assertFalse(self.journal.exists())
        self.assertFalse((self.d / "signal-journal").exists())
        self.state.chmod(0o755)
        self.run_entry("stop", ok=False)
        self.assertFalse(self.journal.exists())
        self.assertFalse((self.d / "signal-journal").exists())
        self.assertTrue((self.state / "alive").exists())

    def test_existing_foreign_recovery_cannot_be_overwritten_by_stop(self):
        self.write_journal()
        self.f.own_provider()
        original = self.journal.read_text().replace(BASE.COOKIE, "a" * 32)
        self.journal.write_text(original)
        self.run_entry("stop", ok=False)
        self.assertEqual(self.journal.read_text(), original)
        self.assertFalse((self.d / "signal-journal").exists())
        self.assertTrue((self.state / "alive").exists())

    def test_pending_or_clean_journal_fences_new_request_and_mount(self):
        (self.state / "latency-runtime.active").unlink()
        self.f.record(mode="relay")
        for phase in ("pending", "clean", "ssr_requested", "ssr_offline", "ssr_ready"):
            with self.subTest(phase=phase):
                self.write_journal(phase=phase)
                before = self.journal.read_bytes()
                self.run_entry("request", ok=False)
                self.run_entry("mount", ok=False)
                self.assertEqual(self.journal.read_bytes(), before)
                self.assertEqual(self.boundaries(), [])
                self.f.no_mount()

    def test_hal_pid_change_alone_does_not_discharge_uncertainty(self):
        self.write_journal()
        (self.state / "hal.pid").write_text("8888\n")
        (self.state / "hal.start").write_text("3333\n")
        self.run_entry("recovered", "8888", ok=False)
        self.assertEqual(self.journal_fields()[8], "pending")
        self.assertTrue((self.state / "latency-runtime.active").exists())
        self.assertFalse(any(line.startswith("normal-ready") for line in self.boundaries()))

    def test_recovery_status_is_read_only_and_never_restarts_services(self):
        self.write_journal()
        before = {p.name: p.read_bytes() for p in self.state.iterdir() if p.is_file()}
        receipt = json.loads(self.run_entry("recovery-status").stdout)
        self.assertTrue(receipt["owned_recovery_required"])
        self.assertEqual(receipt["recovery_phase"], "pending")
        self.assertEqual(
            before, {p.name: p.read_bytes() for p in self.state.iterdir() if p.is_file()}
        )
        self.assertEqual(self.boundaries(), [])

    def test_bad_boot_symlink_owner_mode_and_links_refuse_mutation(self):
        for problem in ("boot", "symlink", "owner", "mode", "hardlink"):
            with self.subTest(problem=problem):
                self.write_journal(boot="invalid-boot" if problem == "boot" else BASE.BOOT)
                extra = None
                if problem == "mode":
                    self.journal.chmod(0o644)
                if problem == "owner":
                    extra = {"TEST_UNSAFE_OWNER_PATH": str(self.journal)}
                if problem in ("symlink", "hardlink"):
                    outside = self.d / (problem + "-keep")
                    outside.write_bytes(self.journal.read_bytes())
                    outside.chmod(0o600)
                    self.journal.unlink()
                    if problem == "symlink":
                        self.journal.symlink_to(outside)
                    else:
                        os.link(outside, self.journal)
                original = self.journal.read_bytes()
                self.run_entry("recover-dsp", ok=False, extra=extra)
                self.assertEqual(self.journal.read_bytes(), original)
                self.assertFalse(self.boundaries())
                self.journal.unlink()

    def test_normal_recovery_requires_offline_then_running_and_fresh_ready_hal(self):
        self.write_journal()
        self.run_entry("recover-dsp")
        fields = self.journal_fields()
        self.assertEqual(fields[6:9], ["7777", "2222", "ssr_ready"])
        self.assertEqual((self.d / "journal-at-ssr").read_text().split()[8], "ssr_requested")
        self.assertEqual(self.boundaries()[:3], ["preflight", "stop-hal 7777 2222", "restart-dsp"])
        self.assertTrue((self.state / "latency-runtime.active").exists())
        self.run_entry("recovered", "7777", ok=False)
        self.assertTrue(self.journal.exists())
        self.run_entry("recovered", "8888", extra={"TEST_NORMAL_READY": "0"}, ok=False)
        self.assertTrue(self.journal.exists())
        self.run_entry("recovered", "8888", extra={"TEST_PREFLIGHT_FAIL": "1"}, ok=False)
        self.assertTrue(self.journal.exists())
        self.run_entry("recovered", "8888")
        self.assertFalse(self.journal.exists())
        self.assertFalse((self.state / "latency-runtime.active").exists())

    def test_failed_stop_ssr_or_state_timeout_keeps_journal_and_no_blind_retry(self):
        cases = (
            {"TEST_STOP_FAIL": "1"},
            {"TEST_SSR_FAIL": "1"},
            {"TEST_DSP_STATES": "running-only"},
            {"TEST_DSP_STATES": "offline-only"},
        )
        for i, extra in enumerate(cases):
            with self.subTest(extra=extra):
                self.write_journal()
                for name in ("recovery-boundaries", "state-reads", "ssr-written", "journal-at-ssr"):
                    p = self.d / name
                    if p.exists():
                        p.unlink()
                self.run_entry("recover-dsp", ok=False, extra=extra)
                self.assertTrue(self.journal.exists())
                self.assertTrue((self.state / "latency-runtime.active").exists())
                calls = self.boundaries().count("restart-dsp")
                self.assertEqual(calls, 0 if i == 0 else 1)
                phase = self.journal_fields()[8]
                if phase == "ssr_requested":
                    self.run_entry("recover-dsp", ok=False, extra=extra)
                    self.assertEqual(self.boundaries().count("restart-dsp"), 1)

    def test_preflight_failure_never_stops_hal_or_requests_ssr(self):
        self.write_journal()
        self.run_entry("recover-dsp", ok=False, extra={"TEST_PREFLIGHT_FAIL": "1"})
        self.assertEqual(self.journal_fields()[8], "pending")
        self.assertEqual(self.boundaries(), ["preflight"])

    def test_candidate_gsl_or_remaining_overlay_blocks_even_a_late_clean_receipt(self):
        for problem in ("candidate", "overlay"):
            with self.subTest(problem=problem):
                self.write_journal()
                (self.d / "live-hash").write_text(
                    self.f.digest if problem == "candidate" else BASE.STOCK
                )
                (self.d / "mountinfo").write_text(
                    "1 2 3 / " + str(self.f.target) + " options\n" if problem == "overlay" else ""
                )
                original = self.journal.read_bytes()
                self.run_entry(
                    "recover-dsp",
                    ok=False,
                    extra={"TEST_RECOVERY_SAFE": "1", "TEST_RECOVERY_EXIT": "0"},
                )
                self.assertEqual(self.journal.read_bytes(), original)
                self.assertEqual(self.boundaries(), [])
                self.assertEqual(self.provider_calls(), [])

    def old_boot_records(self):
        self.f.record("latency-runtime.active", boot=OLD_BOOT, mode="relay")
        self.f.record(boot=OLD_BOOT, mode="relay")
        self.write_journal(boot=OLD_BOOT)
        # These PID numbers belong to another boot. Even a coincident live
        # process with the same numeric identity must never be signalled.
        self.f.own_provider()
        for name in ("latency-provider.pid", "latency-provider.start"):
            (self.state / name).chmod(0o600)
        names = (
            "latency-runtime.active",
            "latency-runtime.request",
            "latency-provider.pid",
            "latency-provider.start",
            "latency-remote-uncertain",
        )
        return {name: (self.state / name).read_bytes() for name in names}

    def assert_records_unchanged(self, records):
        self.assertEqual(records, {name: (self.state / name).read_bytes() for name in records})
        self.assertFalse((self.d / "signal-journal").exists())
        self.assertFalse(
            any(line.startswith(("restart-dsp", "stop-hal")) for line in self.boundaries())
        )

    def test_old_boot_stop_and_status_never_kill_or_reuse_old_pid_authority(self):
        records = self.old_boot_records()
        self.run_entry("stop")
        self.assert_records_unchanged(records)
        status = json.loads(self.run_entry("recovery-status").stdout)
        self.assertTrue(status["owned_recovery_required"])
        self.assertEqual(status["recovery_phase"], "previous_boot")
        self.assert_records_unchanged(records)
        self.assertEqual(self.provider_calls(), [])
        self.run_entry("request", ok=False)
        self.assert_records_unchanged(records)

    def test_old_boot_stock_and_full_current_hal_proof_are_required_without_ssr(self):
        records = self.old_boot_records()
        (self.d / "live-hash").write_text(self.f.digest)
        self.run_entry("recover-dsp", ok=False)
        self.run_entry("recovered", "8888", ok=False)
        self.assert_records_unchanged(records)
        (self.d / "live-hash").write_text(BASE.STOCK)
        self.run_entry("recover-dsp", ok=False, extra={"TEST_PREFLIGHT_FAIL": "1"})
        self.run_entry("recovered", "8888", ok=False, extra={"TEST_NORMAL_READY": "0"})
        self.assert_records_unchanged(records)
        self.assertEqual(self.provider_calls(), [])

    def test_old_boot_is_archived_only_after_verified_current_normal_hal(self):
        records = self.old_boot_records()
        self.run_entry("recover-dsp")
        self.assert_records_unchanged(records)
        self.assertEqual(self.provider_calls(), [])
        self.run_entry("recovered", "8888")
        self.assertIn("normal-ready 8888 0", self.boundaries())
        self.assertFalse(
            any(line.startswith(("restart-dsp", "stop-hal")) for line in self.boundaries())
        )
        self.assertFalse((self.d / "signal-journal").exists())
        for name, content in records.items():
            archive = (
                f"{name}.stale.{OLD_BOOT}.{BASE.COOKIE}"
                if name == "latency-runtime.request"
                else f"{name}.{OLD_BOOT}"
            )
            self.assertFalse((self.state / name).exists())
            self.assertEqual((self.state / archive).read_bytes(), content)
        self.assertFalse(
            json.loads(self.run_entry("recovery-status").stdout)["owned_recovery_required"]
        )
        self.run_entry("request")
        self.assertEqual(
            (self.state / "latency-runtime.request").read_bytes(),
            f"{BASE.BOOT} {BASE.COOKIE} {self.f.digest} relay\n".encode(),
        )
        self.f.no_mount()

    def test_old_boot_foreign_request_or_archive_collision_preserves_every_record(self):
        for problem in ("active_archive", "request_archive", "remote_archive", "foreign_request"):
            with self.subTest(problem=problem):
                records = self.old_boot_records()
                collision = None
                if problem == "foreign_request":
                    self.f.record(cookie="b" * 32, boot=OLD_BOOT, mode="relay")
                    records["latency-runtime.request"] = (
                        self.state / "latency-runtime.request"
                    ).read_bytes()
                else:
                    name = {
                        "active_archive": "latency-runtime.active",
                        "request_archive": "latency-runtime.request",
                        "remote_archive": "latency-remote-uncertain",
                    }[problem]
                    suffix = (
                        f".stale.{OLD_BOOT}.{BASE.COOKIE}"
                        if problem == "request_archive"
                        else f".{OLD_BOOT}"
                    )
                    collision = self.state / (name + suffix)
                    collision.write_bytes(b"prior archive must remain exact")
                self.run_entry("recovered", "8888", ok=False)
                self.assert_records_unchanged(records)
                if collision is not None:
                    self.assertEqual(collision.read_bytes(), b"prior archive must remain exact")
                    collision.unlink()


if __name__ == "__main__":
    unittest.main()
