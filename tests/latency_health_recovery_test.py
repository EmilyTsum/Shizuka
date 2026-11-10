# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the installed health watcher with private, non-Android boundaries."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))


class LatencyHealthRecovery(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-health-")
        self.addCleanup(self.temporary.cleanup)
        self.d = Path(self.temporary.name)
        self.state = self.d / "state"
        self.state.mkdir()
        (self.d / "lib").mkdir()
        (self.state / "active").touch()
        (self.state / "health-token").write_text("current\n")
        (self.state / "hal.pid").write_text("7777\n")
        (self.state / "hal.start").write_text("123\n")
        (self.d / "lib/runtime.sh").write_text(r'''
token_matches() { [ "$(cat "$1")" = "$2" ]; }
process_matches() {
 case "$1" in 4242) [ "${TEST_BT_ALIVE:-1}" = 1 ];;7777) [ "${TEST_HAL_ALIVE:-1}" = 1 ];;*) return 1;;esac
}
getprop() { printf '%s' "${TEST_SHUTDOWN:-}"; }
application_fence() {
 if [ "${TEST_APPLICATION_TRACE:-0}" = 1 ];then echo "application $1" >> "$STATE/events";fi
 if [ "${TEST_APPLICATION_ROTATE:-0}" = 1 ];then echo replacement > "$STATE/health-token";fi
 return "${TEST_APPLICATION_HEALTH_EXIT:-0}"
}
sleep() { echo "sleep $*" >> "$STATE/events";rm -f "$STATE/active"; }
''')
        for name, text in {
            "bb": '#!/bin/sh\n[ "$1" = sh ] || exit 99\nshift\nexec sh "$@"\n',
            "latency-runtime.sh": '#!/bin/sh\necho "health $*" >> "${0%/*}/state/events"\nexit "${TEST_HEALTH_EXIT:-0}"\n',
            "control.sh": '#!/bin/sh\necho "control $*" >> "${0%/*}/state/events"\n',
        }.items():
            (self.d / name).write_text(text)
            (self.d / name).chmod(0o755)
        source = (ROOT / "module/health.sh").read_text()
        source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        (self.d / "health.sh").write_text(source)

    def run_watcher(self, token="current", **options):
        result = subprocess.run(SHELL + [str(self.d / "health.sh"), str(self.d), "4242", "123", token],
                                env=dict(os.environ, **options), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        events = self.state / "events"
        return events.read_text().splitlines() if events.exists() else []

    def test_no_latency_activation_skips_health_and_preserves_normal_watcher(self):
        self.assertEqual(self.run_watcher(TEST_HEALTH_EXIT="2"), ["sleep 2"])

    def test_healthy_runtime_does_not_recover_or_require_active_audio(self):
        (self.state / "latency-runtime.active").touch()
        self.assertEqual(self.run_watcher(), ["health health", "sleep 2"])

    def test_runtime_fault_requests_existing_token_bound_recovery_once(self):
        (self.state / "latency-runtime.active").touch()
        self.assertEqual(self.run_watcher(TEST_HEALTH_EXIT="2"),
                         ["health health", "control rollback-health current"])

    def test_broken_activation_marker_still_checks_health(self):
        (self.state / "latency-runtime.active").symlink_to(self.d / "missing")
        self.assertEqual(self.run_watcher(TEST_HEALTH_EXIT="2"),
                         ["health health", "control rollback-health current"])

    def test_stale_watcher_token_does_not_read_health_or_recover(self):
        (self.state / "latency-runtime.active").touch()
        self.assertEqual(self.run_watcher(token="stale", TEST_HEALTH_EXIT="2"), [])

    def test_shutdown_stops_without_starting_recovery(self):
        (self.state / "latency-runtime.active").touch()
        self.assertEqual(self.run_watcher(TEST_HEALTH_EXIT="2", TEST_SHUTDOWN="shutdown,userrequested"),
                         ["health health"])

    def test_hal_or_bluetooth_exit_keeps_existing_recovery_path(self):
        self.assertEqual(self.run_watcher(TEST_HAL_ALIVE="0"), ["control rollback-health current"])

    def test_unresolved_application_recovers_even_without_a_latency_activation(self):
        self.assertEqual(self.run_watcher(TEST_APPLICATION_TRACE="1", TEST_APPLICATION_HEALTH_EXIT="3"),
                         ["application health", "control rollback-health current"])

    def test_live_bounded_application_owner_is_healthy_while_audio_is_idle(self):
        self.assertEqual(self.run_watcher(TEST_APPLICATION_TRACE="1"), ["application health", "sleep 2"])

    def test_generation_rotation_during_application_reader_drops_stale_recovery(self):
        self.assertEqual(self.run_watcher(TEST_APPLICATION_TRACE="1", TEST_APPLICATION_ROTATE="1", TEST_APPLICATION_HEALTH_EXIT="3"),
                         ["application health"])
        self.assertEqual((self.state / "health-token").read_text(), "replacement\n")


if __name__ == "__main__":
    unittest.main()
