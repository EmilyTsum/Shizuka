# SPDX-License-Identifier: GPL-2.0-only
"""Run the real control facade with isolated Android/root boundaries; no device IO."""

import base64
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools  # noqa: E402

SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))
OP = "01234567-89ab-cdef-0123-456789abcdef"
BOOT = "11111111-2222-3333-abcd-555555555555"


class LatencyControlFacade(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.java_tmp = tempfile.TemporaryDirectory(prefix="shizuka-control-decoder-")
        cls.addClassCleanup(cls.java_tmp.cleanup)
        cls.java, compiler = java_tools()
        harness = Path(cls.java_tmp.name) / "LatencyFallbackCheck.java"
        harness.write_text(r"""package dev.shizuka.settings;
import java.io.*;
import java.util.*;
import java.nio.charset.StandardCharsets;
public final class LatencyFallbackCheck {
 public static void main(String[] args) throws Exception {
  Map<String,Object> values = new HashMap<>();
  BufferedReader input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
  String line;
  while ((line=input.readLine())!=null) {
   String[] fields=line.split("\t",3);
   if(fields.length!=3) throw new AssertionError("Invalid typed JSON fixture");
   Object value;
   switch(fields[0]) {
    case "s": value=new String(Base64.getDecoder().decode(fields[2]),StandardCharsets.UTF_8);break;
    case "i": value=Long.valueOf(fields[2]);break;
    case "b": value=Boolean.valueOf(fields[2]);break;
    case "n": value=null;break;
    default: throw new AssertionError("Unknown JSON type");
   }
   values.put(fields[1],value);
  }
  LatencyApplyState.Status s=LatencyApplyState.decode(values,7,1000);
  if(s.schema!=1 || !s.scope.equals("latency_application") || s.available || s.disableAvailable ||
    s.retrySafe || s.operationKnown || !s.operationKnownReported || !s.currentOperationReported ||
    s.currentOperationId!=null || !LatencyApplyState.uuid(s.currentBootId)) throw new AssertionError("Unsafe fallback DTO");
  String id=args[0].equals("-")?null:args[0];
  for(int hq:new int[]{0,150}) {
   LatencyApplyState.View view=LatencyApplyState.evaluate(new LatencyApplyState.Expected(id,hq,100,7),s,1000,true,false);
   if(view.canApply || view.phase==LatencyApplyState.Phase.APPLIED || view.phase==LatencyApplyState.Phase.DISABLED)
    throw new AssertionError("Fallback enabled or confirmed delay");
  }
  if(LatencyApplyState.reconcileLatest(s,7,1000)) throw new AssertionError("Fallback released ownership");
 }
}
""")
        subprocess.run(
            [
                *compiler,
                "--release",
                "8",
                "-d",
                cls.java_tmp.name,
                str(ROOT / "tools/android-app/src/dev/shizuka/settings/LatencyApplyState.java"),
                str(harness),
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="shizuka-control-facade-")
        self.addCleanup(self.tmp.cleanup)
        self.module = Path(self.tmp.name)
        self.calls = self.module / "calls.jsonl"
        self.env = dict(
            os.environ,
            PATH=str(self.module) + os.pathsep + os.environ["PATH"],
            TEST_BOOT=BOOT,
            TEST_CALLS=str(self.calls),
        )
        control = (ROOT / "module/control.sh").read_text()
        self.assertEqual(control.count("BB=/data/adb/ksu/bin/busybox"), 1)
        (self.module / "control.sh").write_text(
            control.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        )
        self.write_executable(
            "bb",
            f"#!{sys.executable}\n"
            + """import json, os, sys
args = sys.argv[1:]
with open(os.environ['TEST_CALLS'], 'a') as out:
    out.write(json.dumps(['busybox', *args]) + '\\n')
if args[0] not in ('grep', 'sh'):
    raise SystemExit('unexpected Android/lifecycle operation: ' + args[0])
os.execvp(args[0], args)
""",
        )
        self.write_executable(
            "id", f"#!{sys.executable}\nimport os\nprint(os.environ.get('TEST_UID', '0'))\n"
        )
        lib = self.module / "lib"
        lib.mkdir()
        (lib / "runtime.sh").write_text(
            """ensure_busybox_shell() { :; }
lock_lifecycle() { printf 'lock\\n' >> "$MODDIR/lock-called"; exit 73; }
cat() {
 if [ "$#" = 1 ] && [ "$1" = /proc/sys/kernel/random/boot_id ];then
  printf '%s\\n' "$TEST_BOOT"
 else command cat "$@";fi
}
"""
        )
        for name in ("device", "properties", "config", "policy"):
            (lib / (name + ".sh")).write_text("# Isolated Android boundary.\n")
        self.helper = self.module / "latency-apply.sh"
        self.helper_source = (
            "#!/bin/sh\nexec " + shlex.quote(sys.executable) + ' "${0%/*}/helper.py" "$@"\n'
        )
        (self.module / "helper.py").write_text(
            "import json, os, sys\n"
            "with open(os.environ['TEST_CALLS'], 'a') as out:\n"
            " out.write(json.dumps(['helper', *sys.argv[1:]]) + '\\n')\n"
            "print(json.dumps({'forwarded': sys.argv[1:]}))\n"
            "raise SystemExit(int(os.environ.get('TEST_HELPER_EXIT', '0')))\n"
        )

    def write_executable(self, name, content):
        path = self.module / name
        path.write_text(content)
        path.chmod(0o755)
        return path

    def install_helper(self, *, dex=True, sha=True):
        self.helper.write_text(self.helper_source)
        if dex:
            (self.module / "latency-application.dex").write_bytes(b"fixture-dex")
        if sha:
            (self.module / "latency-application.sha256").write_text("fixture-sha\n")

    def run_control(self, *args, ok=True, env=None):
        result = subprocess.run(
            [*SHELL, str(self.module / "control.sh"), *args],
            env=self.env if env is None else env,
            capture_output=True,
            text=True,
            timeout=5,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def trace(self):
        return (
            []
            if not self.calls.exists()
            else [json.loads(line) for line in self.calls.read_text().splitlines()]
        )

    def assert_no_activation(self):
        self.assertFalse((self.module / "lock-called").exists())
        self.assertEqual(
            [
                name
                for name in ("active", "armed", "mounts.tsv", "latency-runtime.request")
                if (self.module / "state" / name).exists()
            ],
            [],
        )
        self.assertTrue(all(call[0] != "helper" for call in self.trace()))
        self.assertTrue(all(call[1] == "grep" for call in self.trace()))

    def test_missing_helper_is_structured_unqualified_and_never_reconciles(self):
        for args, expected_id in (
            (("latency-apply-status",), None),
            (("latency-apply-status", OP), OP),
            (("latency-apply", OP, "150", "100"), OP),
            (("latency-apply", OP, "0", "0"), OP),
        ):
            with self.subTest(args=args):
                result = json.loads(self.run_control(*args).stdout)
                self.assertEqual(result["schema"], 1)
                self.assertEqual(result["scope"], "latency_application")
                self.assertEqual(result["operation_id"], expected_id)
                self.assertEqual(result["current_boot_id"], BOOT)
                self.assertIsNone(result["current_operation_id"])
                self.assertEqual(result["phase"], "unavailable")
                self.assertIs(result["terminal"], True)
                for field in (
                    "operation_known",
                    "available",
                    "current_context_supported",
                    "disable_available",
                    "accepted",
                    "retry_safe",
                    "applied",
                    "disabled",
                    "fresh",
                    "lease_valid",
                    "disable_receipt_valid",
                    "current_config_matches",
                ):
                    self.assertIs(result[field], False, field)
                self.assertIs(result["pending_hq"], True)
                self.assertIs(result["pending_ll"], True)
                self.assertNotIn("requested_hq_ms", result)
                self.assertNotIn("requested_ll_ms", result)
                self.assertIn("ownership has not been reconciled", result["reason"])
        self.assert_no_activation()

    def test_real_fallback_crosses_native_decoder_without_enabling_or_releasing_ownership(self):
        for args in (
            ("latency-apply-status",),
            ("latency-apply-status", OP),
            ("latency-apply", OP, "150", "100"),
        ):
            with self.subTest(args=args):
                result = json.loads(self.run_control(*args).stdout)
                protocol = []
                for key, value in result.items():
                    if value is None:
                        kind, encoded = "n", ""
                    elif isinstance(value, bool):
                        kind, encoded = "b", str(value).lower()
                    elif isinstance(value, int):
                        kind, encoded = "i", str(value)
                    elif isinstance(value, str):
                        kind, encoded = "s", base64.b64encode(value.encode()).decode()
                    else:
                        self.fail("Unexpected fallback JSON type")
                    protocol.append(f"{kind}\t{key}\t{encoded}\n")
                decoded = subprocess.run(
                    [
                        self.java,
                        "-cp",
                        self.java_tmp.name,
                        "dev.shizuka.settings.LatencyFallbackCheck",
                        result["operation_id"] or "-",
                    ],
                    input="".join(protocol),
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assert_no_activation()

    def test_every_helper_prerequisite_is_required(self):
        for present in (
            (),
            ("helper",),
            ("dex",),
            ("sha",),
            ("helper", "dex"),
            ("helper", "sha"),
            ("dex", "sha"),
        ):
            with self.subTest(present=present):
                for file in (
                    self.helper,
                    self.module / "latency-application.dex",
                    self.module / "latency-application.sha256",
                ):
                    file.unlink(missing_ok=True)
                if "helper" in present:
                    self.helper.write_text(self.helper_source)
                if "dex" in present:
                    (self.module / "latency-application.dex").touch()
                if "sha" in present:
                    (self.module / "latency-application.sha256").touch()
                result = json.loads(self.run_control("latency-apply-status").stdout)
                self.assertIs(result["retry_safe"], False)
        self.assert_no_activation()

    def test_symlink_launcher_is_not_executed(self):
        self.install_helper()
        target = self.module / "foreign-launcher.sh"
        self.helper.rename(target)
        self.helper.symlink_to(target)
        result = json.loads(self.run_control("latency-apply", OP, "150", "100").stdout)
        self.assertEqual(result["phase"], "unavailable")
        self.assert_no_activation()

    def test_exact_fixed_argv_and_exit_are_forwarded_without_lifecycle_lock(self):
        self.install_helper()
        for args, forwarded in (
            (("latency-apply", OP, "150", "100"), ["begin", OP, "150", "100"]),
            (("latency-apply", OP, "0", "0"), ["begin", OP, "0", "0"]),
            (("latency-apply-status", OP), ["status", OP]),
            (("latency-apply-status",), ["status"]),
        ):
            with self.subTest(args=args):
                result = self.run_control(*args)
                self.assertEqual(json.loads(result.stdout)["forwarded"], forwarded)
                self.assertEqual(self.trace()[-1], ["helper", *forwarded])
                self.assertEqual(self.trace()[-2], ["busybox", "sh", str(self.helper), *forwarded])
                self.assertFalse((self.module / "lock-called").exists())
        failed = self.run_control(
            "latency-apply-status", OP, ok=False, env=dict(self.env, TEST_HELPER_EXIT="79")
        )
        self.assertEqual(failed.returncode, 79)
        # The trap is live for ordinary lifecycle work: the new entry points did
        # not simply run in a fixture that silently disables every lock.
        self.assertEqual(self.run_control("rollback", ok=False).returncode, 73)
        self.assertEqual((self.module / "lock-called").read_text(), "lock\n")

    def test_malformed_arity_identity_and_values_never_reach_helper(self):
        self.install_helper()
        invalid = [
            ("latency-apply",),
            ("latency-apply", OP),
            ("latency-apply", OP, "0"),
            ("latency-apply", OP, "0", "0", "extra"),
            ("latency-apply-status", OP, "extra"),
        ]
        for bad in (
            "",
            OP.upper(),
            OP[:-1],
            OP + "\n",
            OP[:35] + "\n",
            OP + "\n" + OP,
            OP[:8] + "\n" + OP[9:],
            "../operation",
            "$(touch injected)",
            '"' + OP,
        ):
            invalid.extend((("latency-apply", bad, "0", "0"), ("latency-apply-status", bad)))
        for bad in (
            "",
            "-1",
            "+1",
            "00",
            "01",
            "151",
            "1000",
            "1.0",
            "1e2",
            "true",
            "1\n",
            "1;exit 0",
            "$(id)",
        ):
            invalid.extend((("latency-apply", OP, bad, "0"), ("latency-apply", OP, "0", bad)))
        for args in invalid:
            with self.subTest(args=args):
                result = self.run_control(*args, ok=False)
                self.assertNotIn('"forwarded"', result.stdout)
        self.assert_no_activation()
        self.assertFalse((self.module / "injected").exists())

    def test_invalid_current_boot_cannot_emit_fallback_identity(self):
        for boot in ("", BOOT.upper(), BOOT[:-1], BOOT[:35] + "\n", BOOT[:8] + "\n" + BOOT[9:]):
            with self.subTest(boot=boot):
                result = self.run_control(
                    "latency-apply-status", ok=False, env=dict(self.env, TEST_BOOT=boot)
                )
                self.assertNotIn('"scope"', result.stdout)
        self.assert_no_activation()

    def test_non_root_cannot_read_or_dispatch_the_entry(self):
        self.install_helper()
        result = self.run_control(
            "latency-apply-status", ok=False, env=dict(self.env, TEST_UID="2000")
        )
        self.assertIn("root required", result.stderr)
        self.assertEqual(self.trace(), [])

    def owned_arguments(self):
        return ("2", OP, "150", "0", "180000", "a" * 64, "b" * 64, "0", "321", "456")

    def test_private_actions_are_unavailable_without_the_fixed_owner(self):
        for command in ("latency-owned", "latency-owned-health"):
            with self.subTest(command=command):
                result = self.run_control(command, *self.owned_arguments(), ok=False)
                self.assertEqual(result.returncode, 10)
        self.assert_no_activation()

    def test_private_execution_forwards_exact_arguments_and_result_without_relocking(self):
        owner = self.module / "latency-apply-owned.sh"
        owner.write_text(self.helper_source)
        result = self.run_control("latency-owned", *self.owned_arguments())
        self.assertEqual(json.loads(result.stdout)["forwarded"], ["execute", *self.owned_arguments()])
        self.assertEqual(self.trace()[-2], ["busybox", "sh", str(owner), "execute", *self.owned_arguments()])
        self.assertFalse((self.module / "lock-called").exists())
        result = self.run_control("latency-owned", *self.owned_arguments(), ok=False,
                                  env=dict(self.env, TEST_HELPER_EXIT="30"))
        self.assertEqual(result.returncode, 30)

    def test_private_health_rotation_requires_successful_owner_validation(self):
        (self.module / "latency-apply-owned.sh").write_text(self.helper_source)
        control = self.module / "control.sh"
        source = control.read_text()
        self.assertEqual(source.count("start_health() {\n"), 1)
        # Spy only the health-rotation boundary; run the real dispatch/guard path.
        control.write_text(source.replace("start_health() {\n",
            'start_health() {\n printf "rotated\\n" >> "$MODDIR/health-rotated";return 0\n'))
        for status in (10, 20, 30):
            result = self.run_control("latency-owned-health", *self.owned_arguments(), ok=False,
                                      env=dict(self.env, TEST_HELPER_EXIT=str(status)))
            self.assertEqual(result.returncode, status)
            self.assertFalse((self.module / "health-rotated").exists())
        self.run_control("latency-owned-health", *self.owned_arguments())
        self.assertEqual((self.module / "health-rotated").read_text(), "rotated\n")
        self.assertEqual(self.trace()[-1], ["helper", "validate-health", *self.owned_arguments()])
        self.assertFalse((self.module / "lock-called").exists())

    def test_private_bad_arity_symlink_and_nonroot_never_reach_owner(self):
        owner = self.module / "latency-apply-owned.sh"
        owner.write_text(self.helper_source)
        for command in ("latency-owned", "latency-owned-health"):
            for args in ((), self.owned_arguments()[:-1], (*self.owned_arguments(), "extra")):
                self.run_control(command, *args, ok=False)
            result = self.run_control(command, *self.owned_arguments(), ok=False,
                                      env=dict(self.env, TEST_UID="2000"))
            self.assertIn("root required", result.stderr)
        target = self.module / "external-owner.sh"
        owner.rename(target)
        owner.symlink_to(target)
        for command in ("latency-owned", "latency-owned-health"):
            result = self.run_control(command, *self.owned_arguments(), ok=False)
            self.assertEqual(result.returncode, 10)
        self.assert_no_activation()


if __name__ == "__main__":
    unittest.main()
