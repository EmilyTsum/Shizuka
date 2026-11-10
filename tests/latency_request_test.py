# SPDX-License-Identifier: GPL-2.0-only
"""Exercise request-only staging without mounting or starting audio processes."""

import hashlib
import importlib.util
import os
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "latency_request_lifecycle_fixture", ROOT / "tests/latency_runtime_lifecycle_test.py"
)
FIXTURES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURES)


class LatencyRequest(unittest.TestCase):
    def setUp(self):
        # Compose the fixture rather than inherit its unrelated lifecycle tests.
        self.f = FIXTURES.LatencyLifecycle(methodName="runTest")
        self.addCleanup(self.f.doCleanups)
        self.f.setUp()
        self.d, self.state = self.f.d, self.f.state
        (self.d / "module.prop").write_text("id=s25_aptx_adaptive\n")
        (self.d / "device-profile").write_text("samsung-sm-s931z-dzif\n")
        self.native = self.d / "payload/patched-libbluetooth_jni.so"
        self.hal = self.d / "payload/patched-libsehbluetooth_audio_session_aidl.so"
        self.native.write_bytes(b"synthetic JNI payload, not executable")
        self.hal.write_bytes(b"synthetic HAL payload, not executable")
        self.identity = self.d / "latency-runtime.identity"
        self.identity_bytes = (
            f"1 {FIXTURES.COOKIE} {self.f.digest} {self.sha(self.native)} {self.sha(self.hal)}\n"
        ).encode()
        self.identity.write_bytes(self.identity_bytes)
        self.identity.chmod(0o444)
        # Extend only this private copied boundary, never the shared fixture.
        source = self.f.bb.read_text()
        source = source.replace(
            "p=pathlib.Path(a[3]);s=p.stat();fmt=a[2]",
            "p=pathlib.Path(a[3]);s=p.stat();fmt=a[2]\n"
            ' owner=os.environ.get("TEST_OWNER","0")\n'
            ' if str(p)==os.environ.get("TEST_UNSAFE_OWNER_PATH"):owner="1000"',
        )
        source = source.replace(
            'if fmt=="%u":print(os.environ.get("TEST_OWNER","0"))', 'if fmt=="%u":print(owner)'
        )
        source = source.replace(
            'print(os.environ.get("TEST_OWNER","0")+":"+format(stat.S_IMODE(s.st_mode),"o"))',
            'print(owner+":"+format(stat.S_IMODE(s.st_mode),"o"))',
        )
        source = source.replace(
            'elif fmt=="%u:%g:%a:%h":',
            'elif fmt=="%u:%a:%h":print(owner+":"+format(stat.S_IMODE(s.st_mode),"o")+":"+str(s.st_nlink))\n'
            ' elif fmt=="%s":print(s.st_size)\n'
            ' elif fmt=="%u:%g:%a:%h":',
        )
        self.f.bb.write_text(source)
        (self.d / "id").write_text(
            '#!/bin/sh\ncase "$*" in "-u audioserver") echo 1041;;'
            '"-g audio") echo 1005;;*) echo "${TEST_UID:-0}";;esac\n'
        )

    @staticmethod
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    @property
    def request(self):
        return self.state / "latency-runtime.request"

    def run_request(self, *, ok=True, extra=None, arguments=()):
        return self.f.run_entry("request", *arguments, ok=ok, extra=extra)

    def assert_no_activation(self):
        for name in (
            "latency-runtime.active",
            "latency-provider.pid",
            "latency-provider.start",
            "active",
            "armed",
            "mounts.tsv",
            "experiment-unverified",
        ):
            self.assertFalse(os.path.lexists(self.state / name), name)
        self.assertFalse(self.f.public.exists())
        self.assertFalse((self.d / "provider-launch").exists())
        self.assertFalse((self.state / "signals").exists())
        self.assertFalse((self.d / "proofs").exists())
        self.assertEqual((self.d / "live-hash").read_text(), FIXTURES.STOCK)
        self.assertEqual((self.d / "mountinfo").read_text(), "")
        for event in self.f.events():
            self.assertNotIn("mount", event["args"])
            self.assertNotIn("setsid", event["args"])
        self.assertFalse(list(self.state.glob(".latency-runtime.*")))

    def expected_request(self):
        return f"{FIXTURES.BOOT} {FIXTURES.COOKIE} {self.f.digest} relay\n".encode()

    def test_request_creates_only_current_boot_private_relay_record(self):
        result = self.run_request()
        self.assertEqual(self.request.read_bytes(), self.expected_request())
        self.assertEqual(self.request.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.assertNotIn("applied", result.stdout.lower())
        self.assert_no_activation()

    def test_relay_mount_consumes_the_same_verified_packaged_identity(self):
        self.run_request()
        self.f.run_entry("mount")
        self.assertEqual(
            (self.state / "latency-runtime.active").read_bytes(), self.expected_request()
        )
        self.assertEqual((self.d / "live-hash").read_text(), self.f.digest)

    def test_relay_mount_rejects_payload_replacement_after_request(self):
        self.run_request()
        for path in (self.f.candidate, self.native, self.hal):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"replacement after request")
                self.f.run_entry("mount", ok=False)
                path.write_bytes(original)
                self.assertEqual(self.request.read_bytes(), self.expected_request())
                self.assert_no_activation()

    def test_relay_mount_requires_identity_and_matching_request_cookie(self):
        self.run_request()
        self.identity.unlink()
        self.f.run_entry("mount", ok=False)
        self.assert_no_activation()
        self.identity.write_bytes(self.identity_bytes)
        self.identity.chmod(0o644)
        changed = self.identity_bytes.replace(FIXTURES.COOKIE.encode(), b"a" * 32)
        self.identity.write_bytes(changed)
        self.f.run_entry("mount", ok=False)
        self.assertEqual(self.request.read_bytes(), self.expected_request())
        self.assert_no_activation()

    def test_installer_keeps_optional_provider_executable_and_identity_nonexecutable(self):
        provider = self.d / "latency-provider"
        provider.write_bytes(b"synthetic provider executable fixture")
        capture = self.d / "dsp-capture"
        capture.write_bytes(b"synthetic capture executable fixture")
        script = r'''
set -eu
ui_print() { :; }
set_perm() {
 [ "$2:$3" = 0:0 ] || exit 91
 chmod "$4" "$1"
}
set_perm_recursive() {
 [ "$2:$3" = 0:0 ] || exit 92
 find "$1" -type d -exec chmod "$4" '{}' ';'
 find "$1" -type f -exec chmod "$5" '{}' ';'
}
. "$CUSTOMIZE"
'''
        result = subprocess.run(
            FIXTURES.SHELL + ["-c", script],
            env=dict(self.f.env, MODPATH=str(self.d), CUSTOMIZE=str(ROOT / "module/customize.sh")),
            capture_output=True,
            text=True,
            timeout=6,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(provider.stat().st_mode & 0o777, 0o755)
        self.assertEqual(capture.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.identity.stat().st_mode & 0o777, 0o644)

    def test_same_boot_request_is_idempotent_without_replacement(self):
        self.run_request()
        before = self.request.stat()
        self.run_request()
        after = self.request.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
        self.assertEqual(self.request.read_bytes(), self.expected_request())
        self.assert_no_activation()

    def test_private_state_is_created_and_owned_permissions_are_secured(self):
        self.state.rmdir()
        self.run_request()
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.request.unlink()
        (self.state / "lifecycle.lock").unlink()
        self.state.chmod(0o777)
        self.run_request()
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.assert_no_activation()

    def test_root_is_required_before_writing_request(self):
        self.run_request(ok=False, extra={"TEST_UID": "2000"})
        self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_foreign_state_owner_is_rejected(self):
        self.run_request(ok=False, extra={"TEST_UNSAFE_OWNER_PATH": str(self.state)})
        self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_symlink_state_and_lock_preserve_outside_files(self):
        outside = self.d / "outside"
        outside.mkdir()
        self.state.rmdir()
        self.state.symlink_to(outside, target_is_directory=True)
        self.run_request(ok=False)
        self.assertEqual(list(outside.iterdir()), [])
        self.state.unlink()
        self.state.mkdir(mode=0o700)
        keep = outside / "keep"
        keep.write_bytes(b"keep")
        (self.state / "lifecycle.lock").symlink_to(keep)
        self.run_request(ok=False)
        self.assertEqual(keep.read_bytes(), b"keep")
        self.assertFalse(self.request.exists())

    def test_wrong_module_profile_disabled_removed_and_extra_args_reject(self):
        for name, bad, good in (
            ("module.prop", "id=another_module\n", "id=s25_aptx_adaptive\n"),
            ("device-profile", "other-device\n", "samsung-sm-s931z-dzif\n"),
        ):
            with self.subTest(name=name):
                (self.d / name).write_text(bad)
                self.run_request(ok=False)
                (self.d / name).write_text(good)
                self.assertFalse(self.request.exists())
        for name in ("disable", "remove"):
            (self.d / name).touch()
            self.run_request(ok=False)
            (self.d / name).unlink()
        self.run_request(ok=False, arguments=("relay",))
        self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_identity_must_be_regular_owned_single_link_and_not_symlink(self):
        self.run_request(ok=False, extra={"TEST_UNSAFE_OWNER_PATH": str(self.identity)})
        for kind in ("symlink", "directory", "fifo", "hardlink", "missing"):
            with self.subTest(kind=kind):
                self.identity.unlink()
                outside = self.d / "identity-keep"
                outside.write_bytes(self.identity_bytes)
                if kind == "symlink":
                    self.identity.symlink_to(outside)
                elif kind == "directory":
                    self.identity.mkdir()
                elif kind == "fifo":
                    os.mkfifo(self.identity)
                elif kind == "hardlink":
                    os.link(outside, self.identity)
                self.run_request(ok=False)
                self.assertEqual(outside.read_bytes(), self.identity_bytes)
                self.assertFalse(self.request.exists())
                if kind == "directory":
                    self.identity.rmdir()
                elif kind != "missing":
                    self.identity.unlink()
                outside.unlink()
                self.identity.write_bytes(self.identity_bytes)
                self.identity.chmod(0o444)
        self.assert_no_activation()

    def test_identity_exact_five_fields_version_cookie_digests_and_length(self):
        fields = self.identity_bytes.decode().strip().split()
        invalid = [
            self.identity_bytes.rstrip(b"\n"),
            self.identity_bytes + b"\n",
            self.identity_bytes + b"extra\n",
            b"".join([self.identity_bytes] * 2),
        ]
        for index, value in (
            (0, "2"),
            (1, "0" * 32),
            (1, "A" * 32),
            (1, "a" * 31),
            (2, "g" * 64),
            (3, "F" * 64),
            (4, "0" * 63),
        ):
            changed = list(fields)
            changed[index] = value
            invalid.append((" ".join(changed) + "\n").encode())
        for body in invalid:
            with self.subTest(body=body):
                self.identity.chmod(0o644)
                self.identity.write_bytes(body)
                self.identity.chmod(0o444)
                self.run_request(ok=False)
                self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_identity_modes_444_and_644_allowed_other_modes_rejected(self):
        for mode in (0o444, 0o644):
            self.identity.chmod(mode)
            self.run_request()
            self.request.unlink()
        for mode in (0o400, 0o600, 0o664, 0o666, 0o755):
            with self.subTest(mode=oct(mode)):
                self.identity.chmod(mode)
                self.run_request(ok=False)
                self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_each_packaged_payload_hash_and_safe_file_owner_are_required(self):
        for path in (self.f.candidate, self.native, self.hal):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                self.run_request(ok=False)
                path.write_bytes(original)
                self.run_request(ok=False, extra={"TEST_UNSAFE_OWNER_PATH": str(path)})
                path.chmod(0o666)
                self.run_request(ok=False)
                path.chmod(0o644)
                outside = self.d / "payload-keep"
                path.rename(outside)
                path.symlink_to(outside)
                self.run_request(ok=False)
                path.unlink()
                outside.rename(path)
                self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_unreviewed_gpr_and_osal_dependencies_are_rejected(self):
        for path in ("/vendor/lib64/libar-gpr.so", "/vendor/lib64/liblx-osal.so"):
            with self.subTest(path=path):
                self.run_request(ok=False, extra={"TEST_BAD_DEPENDENCY": path})
                self.assertFalse(self.request.exists())
        self.assert_no_activation()

    def test_any_active_record_including_dangling_symlink_is_preserved(self):
        active = self.state / "latency-runtime.active"
        active.write_bytes(b"unknown active epoch\n")
        self.run_request(ok=False)
        self.assertEqual(active.read_bytes(), b"unknown active epoch\n")
        active.unlink()
        active.symlink_to(self.d / "does-not-exist")
        self.run_request(ok=False)
        self.assertTrue(active.is_symlink())
        self.assertFalse(self.request.exists())
        active.unlink()
        self.assert_no_activation()

    def test_foreign_current_boot_request_is_not_overwritten(self):
        for options in (
            {"cookie": "a" * 32},
            {"digest": "e" * 64},
            {"mode": "observer"},
            {"extra": " unexpected"},
        ):
            with self.subTest(options=options):
                path = self.f.record(**dict({"mode": "relay"}, **options))
                before = path.read_bytes()
                self.run_request(ok=False)
                self.assertEqual(path.read_bytes(), before)
                path.unlink()
        self.assert_no_activation()

    def test_unsafe_request_record_is_not_replaced_or_followed(self):
        self.f.record(mode="relay")
        self.run_request(ok=False, extra={"TEST_UNSAFE_OWNER_PATH": str(self.request)})
        self.request.chmod(0o644)
        self.run_request(ok=False)
        self.request.unlink()
        keep = self.d / "request-keep"
        keep.write_bytes(self.expected_request())
        keep.chmod(0o600)
        self.request.symlink_to(keep)
        self.run_request(ok=False)
        self.assertEqual(keep.read_bytes(), self.expected_request())
        self.request.unlink()
        self.assert_no_activation()

    def test_owned_prior_boot_request_is_archived_then_new_request_is_created(self):
        path = self.f.record(boot="11223344-5566-7788-99aa-bbccddeeff00", mode="relay")
        before = path.read_bytes()
        self.run_request()
        stale = self.state / f"latency-runtime.request.stale.11223344-5566-7788-99aa-bbccddeeff00.{FIXTURES.COOKIE}"
        self.assertEqual(stale.read_bytes(), before)
        self.assertEqual(stale.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.request.read_bytes(), self.expected_request())
        self.assert_no_activation()

    def test_existing_archive_including_dangling_symlink_is_not_overwritten(self):
        self.f.record(boot="11223344-5566-7788-99aa-bbccddeeff00", mode="relay")
        before = self.request.read_bytes()
        stale = self.state / f"latency-runtime.request.stale.11223344-5566-7788-99aa-bbccddeeff00.{FIXTURES.COOKIE}"
        stale.write_bytes(b"prior recovery evidence\n")
        self.run_request(ok=False)
        self.assertEqual(stale.read_bytes(), b"prior recovery evidence\n")
        self.assertEqual(self.request.read_bytes(), before)
        stale.unlink()
        stale.symlink_to(self.d / "missing-archive-target")
        self.run_request(ok=False)
        self.assertTrue(stale.is_symlink())
        self.assertEqual(self.request.read_bytes(), before)
        self.assert_no_activation()

    def test_successive_boots_keep_distinct_stale_requests_without_activation(self):
        boots = (
            "11223344-5566-7788-99aa-bbccddeeff00", FIXTURES.BOOT,
            "22334455-6677-8899-aabb-ccddeeff0011", "33445566-7788-99aa-bbcc-ddeeff001122",
        )
        self.f.record(boot=boots[0], mode="relay")
        for previous, current in zip(boots, boots[1:]):
            before = self.request.read_bytes()
            (self.d / "boot").write_text(current + "\n")
            self.run_request()
            archive = self.state / f"latency-runtime.request.stale.{previous}.{FIXTURES.COOKIE}"
            self.assertEqual(archive.read_bytes(), before)
            self.assertEqual(archive.stat().st_mode & 0o777, 0o600)
            self.assertEqual(
                self.request.read_text(), f"{current} {FIXTURES.COOKIE} {self.f.digest} relay\n"
            )
            self.assert_no_activation()
        self.assertEqual(len(list(self.state.glob("latency-runtime.request.stale.*"))), 3)


if __name__ == "__main__":
    unittest.main()
