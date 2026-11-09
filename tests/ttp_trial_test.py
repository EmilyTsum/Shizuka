#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The experimental builder must refuse unknown input before invoking any tools."""

import io
import struct
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
            for value in (-50, 151, 1000, True, 50.0, "50", None):
                with self.assertRaisesRegex(ValueError, "Invalid"):
                    ttp.compile_patch(value, "not-run", "not-run")
            run.assert_not_called()
        with patch.object(ttp.subprocess, "run") as run:
            for value in (-1, 101, True, 50.0, "50"):
                with self.assertRaisesRegex(ValueError, "Invalid"):
                    ttp.compile_patch(0, "not-run", "not-run", value)
            run.assert_not_called()

    def test_injected_code_has_file_backed_allocated_executable_section(self):
        # A small ELF32/Hexagon fixture checks image metadata independently of
        # the instruction harness, which has its own executable sections.
        end = ttp.CAVE + 64
        strings = b"\0.shstrtab\0"
        section_table = (end + len(strings) + 3) & ~3
        image = bytearray(section_table + 80)
        image[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
        struct.pack_into(
            "<HHIIIIIHHHHHH", image, 16, 3, 164, 1, 0, 52, section_table, 0, 52, 32, 1, 40, 2, 1
        )
        struct.pack_into("<8I", image, 52, 1, 0, 0, 0, end, end, 5, 4096)
        image[ttp.CAVE : end] = bytes(range(64))
        image[end : end + len(strings)] = strings
        struct.pack_into(
            "<10I", image, section_table + 40, 1, 3, 0, 0, end, len(strings), 0, 0, 1, 0
        )
        result = ttp.add_code_section(image, end)
        elf = ttp.ELFFile(io.BytesIO(result))
        section = elf.get_section_by_name(".shizuka.ttp")
        self.assertEqual(section["sh_flags"], 6)
        self.assertEqual(section["sh_addr"], ttp.CAVE)
        self.assertEqual(section.data(), bytes(range(64)))
        self.assertLessEqual(section["sh_addr"] + section["sh_size"], elf.get_segment(0)["p_memsz"])
        self.assertEqual(elf["e_shnum"], 3)
        with self.assertRaises(ValueError):
            ttp.add_code_section(image, len(image) + 1)

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
