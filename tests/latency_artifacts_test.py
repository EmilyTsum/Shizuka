# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: E402
"""Optional runtime artifact identities, using synthetic files and ELF mappings."""

import copy
import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_latency_runtime as builder
from shizuka import latency_artifacts as artifacts
from shizuka.profiles import load_profile
from shizuka.project import digest


def synthetic_hal():
    data = bytearray(0x1300)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    names = b"\0.data\0.symtab\0.strtab\0.shstrtab\0"
    strings = b"\0s25_latency_context\0"
    rva = 0x2000000
    struct.pack_into(
        "<16sHHIQQQIHHHHHH", data, 0, ident, 2, 183, 1, rva, 64, 0x1100, 0, 64, 56, 1, 64, 5, 4
    )
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 6, 0x1000, rva, rva, 128, 128, 0x1000)
    data[0x1000:0x1080] = bytes(range(128))
    struct.pack_into("<IBBHQQ", data, 0x1098, 1, 0x11, 2, 1, rva + 16, 64)
    data[0x10B0 : 0x10B0 + len(strings)] = strings
    data[0x10D0 : 0x10D0 + len(names)] = names
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 3, rva, 0x1000, 128, 0, 0, 16, 0),
        (7, 2, 0, 0, 0x1080, 48, 3, 1, 8, 24),
        (15, 3, 0, 0, 0x10B0, len(strings), 0, 0, 1, 0),
        (23, 3, 0, 0, 0x10D0, len(names), 0, 0, 1, 0),
    ]
    for i, values in enumerate(sections):
        struct.pack_into("<IIQQQQIIQQ", data, 0x1100 + i * 64, *values)
    return bytes(data)


def synthetic_runtime(diagnostic=0, raw_island=0):
    """Small valid ELF with the compiled mode, independent of JSON claims."""
    data = bytearray(0x600)
    names = b"\0.symtab\0.strtab\0.shstrtab\0"
    strings = b"\0shizuka_runtime_diagnostic_zero_ttp_mode\0shizuka_runtime_raw_island_mode\0"
    struct.pack_into(
        "<16sHHIQQQIHHHHHH",
        data,
        0,
        b"\x7fELF\x02\x01\x01" + bytes(9),
        2,
        183,
        1,
        0,
        64,
        0x400,
        0,
        64,
        56,
        1,
        64,
        4,
        3,
    )
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0x500, 0x48000, 0x48000, 64, 64, 0x1000)
    data[0x500:0x540] = bytes([(diagnostic + 2 * raw_island) & 255]) * 64
    data[0x100 : 0x100 + len(strings)] = strings
    data[0x200 : 0x200 + len(names)] = names
    struct.pack_into("<IBBHQQ", data, 0x318, 1, 0x10, 2, 0xFFF1, diagnostic, 0)
    struct.pack_into(
        "<IBBHQQ",
        data,
        0x330,
        strings.index(b"shizuka_runtime_raw_island_mode"),
        0x10,
        2,
        0xFFF1,
        raw_island,
        0,
    )
    for i, values in enumerate(
        [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (1, 2, 0, 0, 0x300, 72, 2, 1, 8, 24),
            (9, 3, 0, 0, 0x100, len(strings), 0, 0, 1, 0),
            (17, 3, 0, 0, 0x200, len(names), 0, 0, 1, 0),
        ]
    ):
        struct.pack_into("<IIQQQQIIQQ", data, 0x400 + i * 64, *values)
    return bytes(data)


class LatencyArtifacts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-artifact-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.profile = load_profile()
        paths = set(artifacts.PROVIDER_COMPONENT_SOURCES) | {
            "experiments/latency/provider/build_provider.py",
            "experiments/latency/native/include/shizuka/external_config.hpp",
            "experiments/latency/runtime/embedded_runtime.c",
            "experiments/latency/transport/native/src/hal_transport.c",
            "experiments/latency/gsl_hook/patch_gsl.py",
            "tools/build_latency_runtime.py",
            "tools/shizuka/latency_artifacts.py",
        }
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic source: " + relative)
        for name in self.profile.data["payloads"].values():
            (self.build / name).write_bytes(b"synthetic core artifact")
        (self.build / "native-build.json").write_text('{"synthetic":true}')
        (self.build / "hal-payload.elf").write_bytes(synthetic_hal())
        (self.build / self.profile.data["payloads"]["hal"]).write_bytes(synthetic_hal())
        # The core build verifier has its own real manifest/source tests. This
        # fixture isolates only the optional artifact boundary and ELF identity.
        mocked = patch.object(artifacts, "verify_native_build")
        mocked.start()
        self.addCleanup(mocked.stop)
        self.seed()

    def seed(self, diagnostic_zero_ttp=False, experimental_raw_island=False):
        self.metadata = artifacts.latency_identity(
            self.profile,
            self.build,
            self.root,
            diagnostic_zero_ttp=diagnostic_zero_ttp,
            experimental_raw_island=experimental_raw_island,
        )
        for name in artifacts.EXTRA_OUTPUTS:
            (self.build / name).write_bytes(("synthetic optional " + name).encode())
        (self.build / "latency-runtime-payload.elf").write_bytes(
            synthetic_runtime(int(diagnostic_zero_ttp), int(experimental_raw_island))
        )
        (self.build / "patched-libar-gsl.so").write_bytes(
            synthetic_runtime(int(diagnostic_zero_ttp), int(experimental_raw_island))
        )
        self.payload = {
            "cookie": self.metadata["cookie"],
            "relay_enabled": True,
            "observer_only": False,
            "attach_enabled": True,
            "hal_context_rva": self.metadata["core"]["hal_context_rva"],
            "sha256": digest(self.build / "latency-runtime-payload.elf"),
            "gpr_initialized_by_runtime": False,
            "device_opened_by_runtime": False,
            "diagnostic_only": diagnostic_zero_ttp or experimental_raw_island,
            "diagnostic_zero_ttp": diagnostic_zero_ttp,
            "experimental_raw_island": experimental_raw_island,
        }
        self.patcher = {
            "stock_sha256": artifacts.REVIEWED_DEPENDENCIES["/vendor/lib64/libar-gsl.so"],
            "candidate_sha256": digest(self.build / "patched-libar-gsl.so"),
            "payload_sha256": digest(self.build / "latency-runtime-payload.elf"),
            "relay_enabled": True,
            "observer_only": False,
            "loaded_on_device": False,
            "new_gpr_transport": False,
        }
        self.provider = {
            "schema": 1,
            "android": True,
            "sha256": digest(self.build / "latency-provider"),
            "gpr_linked": False,
            "device_executed": False,
            "sources_sha256": {
                p: digest(self.root / p) for p in artifacts.PROVIDER_COMPONENT_SOURCES
            },
        }
        for name, value in [
            ("latency-runtime-payload.build.json", self.payload),
            ("latency-runtime-patch.json", self.patcher),
            ("latency-provider.build.json", self.provider),
        ]:
            (self.build / name).write_text(json.dumps(value))
        self.reseal()

    def reseal(self):
        self.metadata["outputs"] = {
            name: digest(self.build / name) for name in artifacts.EXTRA_OUTPUTS
        }
        (self.build / artifacts.LATENCY_BUILD).write_text(json.dumps(self.metadata))

    def verify(self):
        return artifacts.verify_latency_runtime(self.profile, self.build, self.root)

    def test_complete_artifacts_have_deterministic_nonzero_identity_without_activation_or_proof(
        self,
    ):
        self.assertEqual(self.verify(), self.metadata)
        cookie = self.metadata["cookie"]
        self.assertEqual(len(cookie), 32)
        self.assertNotEqual(cookie, "0" * 32)
        self.assertEqual(
            cookie,
            artifacts.latency_cookie(self.profile, self.metadata["sources"], self.metadata["core"]),
        )
        self.assertFalse(self.metadata["activation_requested"])
        self.assertFalse(self.metadata["device_proof"])

    def test_source_groups_and_core_identity_are_independent_and_bound(self):
        for path in [
            "experiments/latency/runtime/embedded_runtime.c",
            "experiments/latency/provider/src/main.cpp",
            "tools/build_latency_runtime.py",
        ]:
            with self.subTest(path=path):
                source = self.root / path
                original = source.read_bytes()
                source.write_bytes(original + b"changed")
                with self.assertRaisesRegex(ValueError, "Stale|source"):
                    self.verify()
                source.write_bytes(original)
        for path in ["native-build.json", self.profile.data["payloads"]["native"], "runtime.prop"]:
            with self.subTest(path=path):
                output = self.build / path
                original = output.read_bytes()
                output.write_bytes(original + b"changed")
                with self.assertRaises(ValueError):
                    self.verify()
                output.write_bytes(original)

    def test_diagnostic_identity_is_distinct_and_production_verifier_rejects_it(self):
        production_cookie = self.metadata["cookie"]
        self.seed(diagnostic_zero_ttp=True)
        self.assertNotEqual(production_cookie, self.metadata["cookie"])
        self.assertTrue(self.metadata["diagnostic_only"])
        self.assertTrue(self.metadata["diagnostic_zero_ttp"])
        with self.assertRaisesRegex(ValueError, "cannot be packaged"):
            self.verify()
        self.assertEqual(
            artifacts.verify_latency_runtime(
                self.profile, self.build, self.root, allow_diagnostic=True
            ),
            self.metadata,
        )
        with self.assertRaisesRegex(ValueError, "cannot be packaged"):
            artifacts.reject_diagnostic_manifest(self.metadata)

    def test_resealed_diagnostic_payload_cannot_hide_behind_production_records(self):
        payload_path = self.build / "latency-runtime-payload.elf"
        payload_path.write_bytes(synthetic_runtime(1))
        for filename, keys in [
            ("latency-runtime-payload.build.json", ["sha256"]),
            ("latency-runtime-patch.json", ["payload_sha256"]),
        ]:
            path = self.build / filename
            metadata = json.loads(path.read_text())
            for key in keys:
                metadata[key] = digest(payload_path)
            path.write_text(json.dumps(metadata))
        self.reseal()
        with self.assertRaisesRegex(ValueError, "compiled diagnostic mode"):
            self.verify()

    def test_all_build_mode_combinations_have_distinct_bound_identities(self):
        cookies = set()
        for zero, raw in ((False, False), (True, False), (False, True), (True, True)):
            with self.subTest(zero=zero, raw=raw):
                self.seed(diagnostic_zero_ttp=zero, experimental_raw_island=raw)
                cookies.add(self.metadata["cookie"])
                self.assertIs(self.metadata["diagnostic_only"], zero or raw)
                self.assertEqual(
                    artifacts.verify_latency_runtime(
                        self.profile, self.build, self.root, allow_diagnostic=True
                    ),
                    self.metadata,
                )
                if zero or raw:
                    with self.assertRaisesRegex(ValueError, "cannot be packaged"):
                        self.verify()
                else:
                    self.assertEqual(self.verify(), self.metadata)
        self.assertEqual(len(cookies), 4)

    def test_resealed_raw_island_payload_and_candidate_cannot_change_mode(self):
        for declared in (False, True):
            with self.subTest(declared=declared):
                self.seed(experimental_raw_island=declared)
                data = synthetic_runtime(0, int(not declared))
                for name in ("latency-runtime-payload.elf", "patched-libar-gsl.so"):
                    (self.build / name).write_bytes(data)
                for name, updates in [
                    (
                        "latency-runtime-payload.build.json",
                        {"sha256": digest(self.build / "latency-runtime-payload.elf")},
                    ),
                    (
                        "latency-runtime-patch.json",
                        {
                            "payload_sha256": digest(self.build / "latency-runtime-payload.elf"),
                            "candidate_sha256": digest(self.build / "patched-libar-gsl.so"),
                        },
                    ),
                ]:
                    path = self.build / name
                    record = json.loads(path.read_text())
                    record.update(updates)
                    path.write_text(json.dumps(record))
                self.reseal()
                with self.assertRaisesRegex(ValueError, "compiled RAW-island mode"):
                    artifacts.verify_latency_runtime(
                        self.profile, self.build, self.root, allow_diagnostic=True
                    )
        self.seed()
        candidate = self.build / "patched-libar-gsl.so"
        candidate.write_bytes(synthetic_runtime(0, 1))
        path = self.build / "latency-runtime-patch.json"
        record = json.loads(path.read_text())
        record["candidate_sha256"] = digest(candidate)
        path.write_text(json.dumps(record))
        self.reseal()
        with self.assertRaisesRegex(ValueError, "embedded in patched GSL"):
            self.verify()

    def test_inconsistent_experimental_metadata_is_rejected_with_explicit_allow(self):
        self.seed(experimental_raw_island=True)
        for key, value in (
            ("diagnostic_only", False),
            ("experimental_raw_island", 1),
            ("diagnostic_zero_ttp", "false"),
        ):
            original = copy.deepcopy(self.metadata)
            self.metadata[key] = value
            self.reseal()
            with self.assertRaisesRegex(ValueError, "Invalid diagnostic"):
                artifacts.verify_latency_runtime(
                    self.profile, self.build, self.root, allow_diagnostic=True
                )
            self.metadata = original
        self.reseal()

    def test_diagnostic_candidate_cannot_hide_behind_an_unrelated_normal_payload(self):
        candidate = self.build / "patched-libar-gsl.so"
        candidate.write_bytes(synthetic_runtime(1))
        path = self.build / "latency-runtime-patch.json"
        metadata = json.loads(path.read_text())
        metadata["candidate_sha256"] = digest(candidate)
        path.write_text(json.dumps(metadata))
        self.reseal()
        with self.assertRaisesRegex(ValueError, "embedded in patched GSL"):
            self.verify()

    def test_diagnostic_modes_are_exact_and_component_flags_are_bound(self):
        artifacts.reject_diagnostic_manifest(self.metadata)
        for key in ("diagnostic_only", "diagnostic_zero_ttp", "experimental_raw_island"):
            for value in (True, 0, 1, None, "false"):
                with self.subTest(key=key, value=value):
                    changed = copy.deepcopy(self.metadata)
                    changed[key] = value
                    with self.assertRaises(ValueError):
                        artifacts.reject_diagnostic_manifest(changed)
            changed = copy.deepcopy(self.metadata)
            del changed[key]
            with self.assertRaises(ValueError):
                artifacts.reject_diagnostic_manifest(changed)
        path = self.build / "latency-runtime-payload.build.json"
        original = path.read_bytes()
        for key in ("diagnostic_only", "diagnostic_zero_ttp", "experimental_raw_island"):
            changed = json.loads(original)
            changed[key] = True
            path.write_text(json.dumps(changed))
            self.reseal()
            with self.assertRaisesRegex(ValueError, "build flags"):
                self.verify()
        path.write_bytes(original)
        self.reseal()
        payload = self.build / "latency-runtime-payload.elf"
        for value in (0, 1):
            payload.write_bytes(synthetic_runtime(value))
            self.assertEqual(artifacts.runtime_diagnostic_zero_ttp(payload), bool(value))
        payload.write_bytes(synthetic_runtime(2))
        with self.assertRaisesRegex(ValueError, "exact compiled diagnostic mode"):
            artifacts.runtime_diagnostic_zero_ttp(payload)
        for value in (0, 1):
            payload.write_bytes(synthetic_runtime(0, value))
            self.assertIs(artifacts.runtime_experimental_raw_island(payload), bool(value))
        payload.write_bytes(synthetic_runtime(0, 2))
        with self.assertRaisesRegex(ValueError, "exact compiled diagnostic mode"):
            artifacts.runtime_experimental_raw_island(payload)

    def test_missing_modified_or_linked_outputs_are_rejected(self):
        for name in [artifacts.LATENCY_BUILD, *artifacts.EXTRA_OUTPUTS]:
            with self.subTest(name=name):
                path = self.build / name
                original = path.read_bytes()
                path.unlink()
                with self.assertRaises(ValueError):
                    self.verify()
                path.write_bytes(original + b"changed")
                with self.assertRaises(ValueError):
                    self.verify()
                path.unlink()
                outside = self.root / "linked-output"
                outside.write_bytes(original)
                path.symlink_to(outside)
                with self.assertRaises(ValueError):
                    self.verify()
                path.unlink()
                path.write_bytes(original)

    def test_metadata_cannot_change_firmware_cookie_mode_or_inherit_device_proof(self):
        for key, value in [
            ("cookie", "1" * 32),
            ("dependencies", {}),
            ("runtime_mode", "observer"),
            ("activation_requested", True),
            ("device_proof", True),
            ("sources", {}),
        ]:
            with self.subTest(key=key):
                metadata = copy.deepcopy(self.metadata)
                metadata[key] = value
                (self.build / artifacts.LATENCY_BUILD).write_text(json.dumps(metadata))
                with self.assertRaises(ValueError):
                    self.verify()
                self.reseal()

    def test_resealed_component_records_still_require_matching_flags_context_and_sources(self):
        for filename, key, value in [
            ("latency-runtime-payload.build.json", "cookie", "2" * 32),
            ("latency-runtime-payload.build.json", "hal_context_rva", 0x9990),
            ("latency-runtime-payload.build.json", "relay_enabled", False),
            ("latency-runtime-patch.json", "stock_sha256", "f" * 64),
            ("latency-runtime-patch.json", "loaded_on_device", True),
            ("latency-provider.build.json", "android", False),
            ("latency-provider.build.json", "sources_sha256", {}),
        ]:
            with self.subTest(filename=filename, key=key):
                path = self.build / filename
                original = path.read_bytes()
                changed = json.loads(original)
                changed[key] = value
                path.write_text(json.dumps(changed))
                self.reseal()
                with self.assertRaises(ValueError):
                    self.verify()
                path.write_bytes(original)
                self.reseal()

    def test_hal_payload_must_be_exactly_embedded_in_matching_core_output(self):
        self.assertEqual(artifacts.hal_context_rva(self.build / "hal-payload.elf"), 0x2000010)
        hal = self.build / self.profile.data["payloads"]["hal"]
        original = hal.read_bytes()
        changed = bytearray(original)
        changed[0x1001] ^= 1
        hal.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, "embedded"):
            self.verify()
        hal.write_bytes(original)
        payload = self.build / "hal-payload.elf"
        changed = bytearray(payload.read_bytes())
        struct.pack_into("<Q", changed, 0x1098 + 16, 128)
        payload.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, "context"):
            artifacts.hal_context_rva(payload)

    def test_hal_context_rejects_wrong_architecture_permissions_or_alignment(self):
        path = self.build / "hal-payload.elf"
        original = path.read_bytes()
        for offset, fmt, value in [(18, "<H", 62), (68, "<I", 5), (0x1098 + 8, "<Q", 0x2000011)]:
            with self.subTest(offset=offset):
                changed = bytearray(original)
                struct.pack_into(fmt, changed, offset, value)
                path.write_bytes(changed)
                with self.assertRaises(ValueError):
                    artifacts.hal_context_rva(path)
        path.write_bytes(original)

    def test_package_copy_has_separate_identity_and_keeps_core_proof_bytes(self):
        spec = importlib.util.spec_from_file_location(
            "package_latency_fixture", ROOT / "tools/package.py"
        )
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        stage = self.root / "stage"
        (stage / "payload").mkdir(parents=True)
        (stage / "proofs").mkdir()
        core = b"four existing core manifest entries\n"
        (stage / "payloads.sha256").write_bytes(core)
        (stage / "proofs/offline-ready").write_text(hashlib.sha256(core).hexdigest() + "\n")
        package.copy_latency_runtime(self.verify(), self.build, stage)
        self.assertEqual((stage / "payloads.sha256").read_bytes(), core)
        self.assertEqual(
            (stage / "proofs/offline-ready").read_text(), hashlib.sha256(core).hexdigest() + "\n"
        )
        self.assertEqual(json.loads((stage / artifacts.LATENCY_BUILD).read_text()), self.metadata)
        self.assertEqual(
            (stage / "latency-provider.sha256").read_text(),
            digest(stage / "latency-provider") + "\n",
        )
        self.assertEqual(
            (stage / "latency-runtime.identity").read_text(),
            f"1 {self.metadata['cookie']} {digest(stage / 'payload/patched-libar-gsl.so')} "
            f"{self.metadata['core']['outputs']['native']} {self.metadata['core']['outputs']['hal']}\n",
        )
        self.assertEqual((stage / "latency-runtime.identity").stat().st_size, 230)
        self.assertEqual((stage / "latency-runtime.identity").stat().st_mode & 0o777, 0o644)
        self.assertFalse((stage / "state").exists())
        self.assertFalse((stage / "proofs/verified-payloads.sha256").exists())

    def test_packaged_shell_identity_requires_the_verified_native_hal_pair(self):
        spec = importlib.util.spec_from_file_location(
            "package_latency_identity_fixture", ROOT / "tools/package.py"
        )
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        self.assertEqual(len(package.latency_runtime_identity(self.verify()).encode()), 230)
        for field in ("cookie", "gsl", "native", "hal"):
            for bad in ("", "F" * (32 if field == "cookie" else 64), None):
                with self.subTest(field=field, bad=bad):
                    metadata = copy.deepcopy(self.metadata)
                    if field == "cookie":
                        metadata["cookie"] = bad
                    elif field == "gsl":
                        metadata["outputs"]["patched-libar-gsl.so"] = bad
                    else:
                        metadata["core"]["outputs"][field] = bad
                    with self.assertRaisesRegex(ValueError, "packaged latency runtime identity"):
                        package.latency_runtime_identity(metadata)
        metadata = copy.deepcopy(self.metadata)
        metadata["cookie"] = "0" * 32
        with self.assertRaises(ValueError):
            package.latency_runtime_identity(metadata)
        del metadata["core"]["outputs"]["native"]
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            package.latency_runtime_identity(metadata)

    def test_direct_package_copy_and_shell_identity_reject_diagnostic_manifests(self):
        spec = importlib.util.spec_from_file_location(
            "package_diagnostic_fixture", ROOT / "tools/package.py"
        )
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        for key in ("diagnostic_only", "diagnostic_zero_ttp", "experimental_raw_island"):
            for value in (True, 1, None):
                with self.subTest(key=key, value=value):
                    metadata = copy.deepcopy(self.metadata)
                    metadata[key] = value
                    stage = self.root / "must-not-be-created"
                    with self.assertRaisesRegex(ValueError, "cannot be packaged"):
                        package.latency_runtime_identity(metadata)
                    with self.assertRaisesRegex(ValueError, "cannot be packaged"):
                        package.copy_latency_runtime(metadata, self.build, stage)
                    self.assertFalse(stage.exists())


class LatencyBuilder(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-builder-test-")
        self.addCleanup(self.temporary.cleanup)
        self.d = Path(self.temporary.name)
        self.build = self.d / "build"
        self.build.mkdir()
        self.compiler = self.d / "ndk"
        self.compiler.mkdir()
        for name in ("aarch64-linux-android34-clang", "aarch64-linux-android34-clang++"):
            (self.compiler / name).touch()
        (self.build / "hal-payload.elf").write_bytes(b"synthetic already-verified HAL fixture")
        (self.build / artifacts.LATENCY_BUILD).write_text("old successful generation")
        self.gsl = self.d / "libar-gsl.so"
        self.gsl.write_bytes(b"synthetic GSL fixture")
        self.identity = {"cookie": "abcd" * 8, "synthetic": True}
        self.calls = []

    def fake_run(self, command, *, env=None):
        command = [str(a) for a in command]
        self.calls.append((command, env))
        if "--output" not in command:
            return
        output = Path(command[command.index("--output") + 1])
        if command[1].endswith("runtime/build_runtime.py"):
            self.assertIn("--relay", command)
            self.assertNotIn("--attach", command)
            self.assertEqual(command[command.index("--cookie") + 1], self.identity["cookie"])
            self.assertEqual(
                Path(command[command.index("--hal-payload") + 1]).read_bytes(),
                (self.build / "hal-payload.elf").read_bytes(),
            )
        output.write_bytes(b"synthetic builder result")
        suffix = ".manifest.json" if command[1].endswith("patch_runtime.py") else ".build.json"
        output.with_suffix(suffix).write_text('{"synthetic":true}')

    def invoke(self, identities=None, reviewed=True, diagnostic=False, raw=False):
        with (
            patch.object(builder, "BUILD", self.build),
            patch.object(builder, "ROOT", self.d),
            patch.object(
                builder,
                "latency_identity",
                side_effect=identities or [self.identity, self.identity],
            ),
            patch.object(
                builder, "reviewed_input", return_value=b"synthetic reviewed input"
            ) as read,
            patch.object(builder, "run", side_effect=self.fake_run),
            patch.object(builder, "verify_latency_runtime", return_value={}),
            patch("builtins.print"),
        ):
            if not reviewed:
                read.side_effect = ValueError("unreviewed dependency")
            builder.main(
                ["--gsl", str(self.gsl), "--ndk-bin", str(self.compiler)]
                + (["--diagnostic-zero-ttp"] if diagnostic else [])
                + (["--experimental-raw-island"] if raw else [])
            )

    def test_orchestrates_only_offline_builds_and_publishes_completion_last(self):
        self.invoke()
        metadata = json.loads((self.build / artifacts.LATENCY_BUILD).read_text())
        self.assertEqual(metadata["cookie"], self.identity["cookie"])
        self.assertEqual(set(metadata["outputs"]), set(artifacts.EXTRA_OUTPUTS))
        self.assertEqual(len(self.calls), 5)
        runtime_test = self.calls[2]
        self.assertTrue(runtime_test[0][1].endswith("runtime/tests/test_runtime.py"))
        self.assertIn("SHIZUKA_GSL_STOCK", runtime_test[1])
        self.assertIn("SHIZUKA_RUNTIME_PAYLOAD", runtime_test[1])
        self.assertIn("--host", self.calls[3][0])
        self.assertFalse(
            any("adb" in arg or "activate" in arg for command, _ in self.calls for arg in command)
        )
        self.assertFalse((self.build / "state").exists())

    def test_diagnostic_flag_reaches_embedded_build_only_when_explicit(self):
        self.invoke(diagnostic=True)
        self.assertIn("--diagnostic-zero-ttp", self.calls[0][0])
        self.calls.clear()
        self.invoke()
        self.assertNotIn("--diagnostic-zero-ttp", self.calls[0][0])

    def test_raw_island_and_zero_flags_are_independent(self):
        for zero, raw in ((False, False), (True, False), (False, True), (True, True)):
            with self.subTest(zero=zero, raw=raw):
                self.calls.clear()
                self.invoke(diagnostic=zero, raw=raw)
                self.assertEqual("--diagnostic-zero-ttp" in self.calls[0][0], zero)
                self.assertEqual("--experimental-raw-island" in self.calls[0][0], raw)

    def test_changed_sources_invalidate_old_completion_and_never_publish_outputs(self):
        with self.assertRaisesRegex(ValueError, "changed"):
            self.invoke(identities=[self.identity, {"cookie": "bad"}])
        self.assertFalse((self.build / artifacts.LATENCY_BUILD).exists())
        self.assertFalse((self.build / "latency-provider").exists())

    def test_unreviewed_dependency_prevents_any_build_and_invalidates_old_completion(self):
        with self.assertRaisesRegex(ValueError, "unreviewed"):
            self.invoke(reviewed=False)
        self.assertEqual(self.calls, [])
        self.assertFalse((self.build / artifacts.LATENCY_BUILD).exists())

    def test_input_hash_size_and_symlink_boundaries(self):
        target = "/vendor/lib64/libar-gsl.so"
        with patch.dict(builder.REVIEWED_DEPENDENCIES, {target: digest(self.gsl)}):
            self.assertEqual(builder.reviewed_input(self.gsl, target), self.gsl.read_bytes())
            linked = self.d / "linked"
            linked.symlink_to(self.gsl)
            with self.assertRaises(ValueError):
                builder.reviewed_input(linked, target)
            self.gsl.write_bytes(b"changed")
            with self.assertRaises(ValueError):
                builder.reviewed_input(self.gsl, target)
            self.gsl.write_bytes(b"")
            with self.assertRaises(ValueError):
                builder.reviewed_input(self.gsl, target)


if __name__ == "__main__":
    unittest.main()
