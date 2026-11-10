#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare native byte output with a frozen Python oracle using generated data."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys


def load(path):
    spec = importlib.util.spec_from_file_location("ttp_oracle", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def packet(oracle, kind=1, role=0xA0, timestamp_units=24625):
    return struct.pack("<H6B", timestamp_units, 120, kind, role, 0, 0, 0xAE) + bytes(
        (index * 17 + 3) & 255 for index in range(oracle.R3_PAYLOAD_BYTES[kind]))


def record(mid, payload, flags=0x21, offset=0):
    return struct.pack("<6I", mid, flags, offset, 0, 0, len(payload)) + payload


def metadata(oracle, data, timestamp=-0x123456789, *, bt=True):
    timestamp &= 0xFFFFFFFFFFFFFFFF
    prefix = b""
    if bt:
        prefix = record(oracle.MD_PCM_FRAME_LENGTH, struct.pack("<I", 1440), offset=2)
        prefix += record(oracle.MD_PCM_MEDIA_FORMAT, struct.pack("<IHH16H", 48000, 24, 2, 1, 2, *([0] * 14)))
        prefix += record(oracle.MD_ZERO_PADDING, struct.pack("<I", 0))
    return prefix + record(oracle.MD_FRAME_INFO, struct.pack("<4I", len(data), timestamp & 0xFFFFFFFF,
                                                            timestamp >> 32, 0x80000000), flags=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    oracle = load(args.oracle)
    root = args.artifacts.resolve()
    root.mkdir(parents=True, exist_ok=False)
    good, bad = 0, 0

    def invoke(data, md, delta, name, *, profile=oracle.SUPPORTED_PROFILE, mapping="physical"):
        folder = root / name
        folder.mkdir()
        data_file, md_file = folder / "rd-data.bin", folder / "rd-metadata.bin"
        data_file.write_bytes(data)
        md_file.write_bytes(md)
        output = folder / "result"
        command = [str(args.binary.resolve()), "--data", str(data_file), "--metadata", str(md_file),
                   "--delta-us", str(delta), "--profile", profile, "--output-dir", str(output)]
        if mapping:
            command += ["--address-mode", mapping,
                        "--data-address", str(0x123456780 if mapping == "physical" else 0),
                        "--data-map-handle", "7", "--data-capacity", "4096",
                        "--metadata-address", str((0x234567880 if mapping == "physical" else 64) if bt else 0),
                        "--metadata-map-handle", "8" if bt else "0", "--metadata-capacity", "4096" if bt else "0"]
        completed = subprocess.run(command, text=True, capture_output=True, timeout=15)
        return completed, output, command

    def compare(data, md, delta, name, *, timestamp=-0x123456789, mapping="physical"):
        nonlocal good, bt
        expected = oracle.relay_buffer(data, md, delta, active_profile=oracle.SUPPORTED_PROFILE)
        bt = bool(expected.metadata)
        completed, output, command = invoke(data, md, delta, name, mapping=mapping)
        if completed.returncode:
            raise AssertionError(completed.stderr)
        assert (output / "wr-data.bin").read_bytes() == expected.data
        assert (output / "wr-metadata.bin").read_bytes() == expected.metadata
        report = json.loads((output / "relay-report.json").read_text())
        assert report["timestamp_us"] == timestamp
        assert int(report["timestamp_bits"]) == timestamp & 0xFFFFFFFFFFFFFFFF
        assert report["wr_flags"] == expected.wr_flags
        if mapping == "physical":
            body = oracle.build_wr_data_command(expected, data_address=0x123456780, data_map_handle=7,
                                                metadata_address=0x234567880 if bt else 0,
                                                metadata_map_handle=8 if bt else 0)
        else:
            # Public offset mode uses the same 64-bit wire fields. Its map was
            # declared separately; offset zero is valid and not a physical IPA.
            body = struct.pack("<11I", 0, 0, 7, len(data), timestamp & 0xFFFFFFFF,
                               (timestamp >> 32) & 0xFFFFFFFF, expected.wr_flags,
                               64 if bt else 0, 0, 8 if bt else 0, len(expected.metadata))
        assert (output / "wr-command.bin").read_bytes() == body
        again = subprocess.run(command, text=True, capture_output=True, timeout=15)
        assert again.returncode == 2 and (output / "wr-data.bin").read_bytes() == expected.data
        assert not list(output.parent.glob(".shizuka-latency-*"))
        good += 1

    bt = True
    for kind in range(9):
        for role in (0, 0x80, 0xA0, 0xC0):
            for delta in (0, 50000, 150000):
                data = packet(oracle, kind, role, 0 if role == 0xC0 else 24625)
                compare(data, metadata(oracle, data), delta, f"type-{kind}-role-{role}-delta-{delta}")

    pair = packet(oracle) + packet(oracle, role=0xC0, timestamp_units=0)
    for timestamp in (0, 0xFFFFFFFF, 0x100000000, (1 << 63) - 1, -(1 << 63), -1):
        for mode in ("physical", "offset"):
            compare(pair, metadata(oracle, pair, timestamp), 150000, f"timestamp-{timestamp}-{mode}", timestamp=timestamp, mapping=mode)
    compare(pair, metadata(oracle, pair, bt=False), 150000, "frame-info-only")

    cases = [
        (b"", b"", 0), (pair[:-1], metadata(oracle, pair), 150000),
        (packet(oracle, role=0xC0, timestamp_units=1), metadata(oracle, packet(oracle)), 150000),
        (pair, metadata(oracle, pair) + record(0x0A001234, bytes(4)), 150000),
        (pair, metadata(oracle, pair) + record(0x0A00100C, bytes(4)), 150000),
        (pair, metadata(oracle, pair)[:-1], 150000),
        (pair, metadata(oracle, pair), -1), (pair, metadata(oracle, pair), 7),
        (pair, metadata(oracle, pair), 524288), (pair, metadata(oracle, pair), (1 << 64) - 8),
        (pair, metadata(oracle, pair), 1 << 64),
    ]
    for flag in (0x02, 0x40, 0x80):
        md = bytearray(metadata(oracle, pair)); struct.pack_into("<I", md, 4, flag)
        cases.append((pair, bytes(md), 150000))
    for index, (data, md, delta) in enumerate(cases):
        try:
            oracle.relay_buffer(data, md, delta, active_profile=oracle.SUPPORTED_PROFILE)
        except oracle.RelayError:
            pass
        else:
            raise AssertionError("negative fixture must be rejected by the oracle")
        result, output, _ = invoke(data, md, delta, f"reject-{index}", mapping=None)
        assert result.returncode == 2 and not output.exists()
        assert not list(output.parent.glob(".shizuka-latency-*"))
        bad += 1

    # File boundaries are independent from packet validity.
    folder = root / "filesystem"; folder.mkdir()
    data_file = folder / "data.bin"; data_file.write_bytes(pair)
    md_file = folder / "metadata.bin"; md_file.write_bytes(metadata(oracle, pair))
    symlink = folder / "data-link.bin"; symlink.symlink_to(data_file)
    for name, file in (("symlink", symlink), ("oversize", folder / "large.bin")):
        if name == "oversize":
            with file.open("wb") as stream: stream.truncate(4 * 1024 * 1024 + 1)
        target = folder / (name + "-result")
        result = subprocess.run([str(args.binary.resolve()), "--data", str(file), "--metadata", str(md_file),
                                 "--delta-us", "150000", "--profile", oracle.SUPPORTED_PROFILE,
                                 "--output-dir", str(target)], capture_output=True, timeout=15)
        assert result.returncode == 2 and not target.exists()
        bad += 1

    report = {"schema": 1, "native_binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "python_oracle_sha256": hashlib.sha256(args.oracle.read_bytes()).hexdigest(),
              "synthetic_positive_cases": good, "synthetic_rejection_cases": bad,
              "data_metadata_and_44B_command_match": True,
              "physical_mode_matches_python_oracle": True, "offset_zero_mode_matches_public_wire_layout": True,
              "existing_output_and_symlink_input_refused": True,
              "input_fixture_provenance": "generated synthetic bytes only; no private capture",
              "graph_or_receiver_validation": False}
    (root / "comparison-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
