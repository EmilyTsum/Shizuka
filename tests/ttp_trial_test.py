#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The experimental builder must refuse unknown input before invoking any tools."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from shizuka.dsp_backends import samsung_dzif_ttp as ttp


class TtpTrialGuards(unittest.TestCase):
    def test_unknown_images_fail_before_elf_parsing(self):
        with patch.object(ttp, "ELFFile") as elf:
            for data in (b"", b"not ELF", b"\x7fELF" + bytes(180000)):
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    ttp.inspect_stock(data)
            elf.assert_not_called()

    def test_build_refuses_unknown_input_without_side_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.so"
            source.write_bytes(b"different firmware")
            with patch.object(ttp.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    ttp.write_candidate(source, root / "result", 50, "not-run", "not-run")
                run.assert_not_called()
            self.assertFalse((root / "result").exists())

    def test_direct_patch_cannot_bypass_stock_gate(self):
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            ttp.patch_verified(b"already patched or wrong firmware", 50, {})

    def test_only_explicit_stages_are_accepted(self):
        with patch.object(ttp.subprocess, "run") as run:
            for value in (-50, 101, 1000, True, 50.0, "50", None):
                with self.assertRaisesRegex(ValueError, "Invalid"):
                    ttp.compile_patch(value, "not-run", "not-run")
            run.assert_not_called()
        with patch.object(ttp.subprocess, "run") as run:
            for value in (-1, 101, True, 50.0, "50"):
                with self.assertRaisesRegex(ValueError, "Invalid"):
                    ttp.compile_patch(0, "not-run", "not-run", value)
            run.assert_not_called()

    def test_existing_output_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.so"
            source.write_bytes(b"fixture")
            output = root / "result"
            output.mkdir()
            mounted = output / "aptx_adaptive_enc_module.so.1"
            mounted.write_bytes(b"must remain unchanged")
            # Only substitute construction here: the other tests exercise the
            # real rejection gate; real-image construction is checked by QEMU.
            with (
                patch.object(ttp, "inspect_stock"),
                patch.object(ttp, "compile_patch", return_value={}),
                patch.object(ttp, "patch_verified", return_value=(b"new", {})),
                self.assertRaises(FileExistsError),
            ):
                ttp.write_candidate(source, output, 50, "not-run", "not-run")
            self.assertEqual(mounted.read_bytes(), b"must remain unchanged")

    def test_zero_stage_is_byte_identical_and_uses_no_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.so"
            source.write_bytes(b"stock fixture")
            with (
                patch.object(ttp, "inspect_stock", return_value={"live_targets": None}),
                patch.object(ttp.subprocess, "run") as run,
            ):
                manifest = ttp.write_candidate(source, root / "result", 0, None, None)
                run.assert_not_called()
            self.assertEqual(
                (root / "result/aptx_adaptive_enc_module.so.1").read_bytes(), source.read_bytes()
            )
            self.assertEqual(manifest["modified_ranges"], [])
            self.assertFalse(manifest["installed"])
            self.assertFalse(manifest["audio_verified"])


class TtpBackendDispatch(unittest.TestCase):
    def test_unknown_profile_has_no_samsung_fallback(self):
        from shizuka.ttp import resolve_backend

        for profile in ("another-phone", "samsung-sm-s931z-another-firmware"):
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                resolve_backend(profile=profile)

    def test_unknown_image_has_no_backend_fallback(self):
        from shizuka.ttp import inspect_stock

        with self.assertRaisesRegex(ValueError, "Unsupported"):
            inspect_stock(b"unknown firmware")

    def test_zero_patch_uses_no_tool(self):
        with patch.object(ttp.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "nonzero"):
                ttp.compile_patch(0, "not-run", "not-run", 0)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
