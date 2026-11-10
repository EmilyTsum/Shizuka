#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Cross-check the freestanding C in-place API with generated synthetic bytes."""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import struct
import subprocess
import sys


class Result(ctypes.Structure):
    _fields_ = [("timestamp_bits", ctypes.c_uint64)] + [
        (name, ctypes.c_uint32) for name in (
            "wr_flags", "data_bytes", "metadata_bytes", "wrapper_count",
            "changed_wrapper_count", "timing_wrapper_count", "secondary_zero_count",
            "first_ttp_before_us", "first_ttp_after_us", "last_ttp_before_us", "last_ttp_after_us")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--oracle", required=True, type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location("encoded_oracle", args.oracle)
    oracle = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = oracle
    spec.loader.exec_module(oracle)
    library = ctypes.CDLL(str(args.library.resolve()))
    transform = library.shz_encoded_transform
    transform.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                          ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                          ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(Result)]
    transform.restype = ctypes.c_int
    positive = negative = mutations = 0

    def packet(kind=1, role=0xA0, units=24625):
        return struct.pack("<H6B", units, 120, kind, role, 0, 0, 0xAE) + bytes(
            (index * 17 + 3) & 255 for index in range(oracle.R3_PAYLOAD_BYTES[kind]))

    def record(mid, payload, flags=0x21, offset=0):
        return struct.pack("<6I", mid, flags, offset, 0, 0, len(payload)) + payload

    def metadata(data, timestamp=-0x123456789, bt=True):
        timestamp &= 0xFFFFFFFFFFFFFFFF
        prefix = b""
        if bt:
            prefix = record(oracle.MD_PCM_FRAME_LENGTH, struct.pack("<I", 1440), offset=2)
            prefix += record(oracle.MD_PCM_MEDIA_FORMAT, struct.pack("<IHH16H", 48000, 24, 2, 1, 2, *([0] * 14)))
            prefix += record(oracle.MD_ZERO_PADDING, struct.pack("<I", 0))
        return prefix + record(oracle.MD_FRAME_INFO, struct.pack("<4I", len(data), timestamp & 0xFFFFFFFF,
                                                                timestamp >> 32, 0x80000000), flags=0)

    def invoke(data, md, delta, profile=1, capacity=None):
        d = ctypes.create_string_buffer(data, max(1, len(data)))
        m = ctypes.create_string_buffer(md, max(1, len(md)))
        out = Result()
        ctypes.memset(ctypes.byref(out), 0x5A, ctypes.sizeof(out))
        previous_result = bytes(out)
        rc = transform(d, len(data), len(data) if capacity is None else capacity,
                       m, len(md), len(md), delta, profile, ctypes.byref(out))
        if rc:
            assert d.raw[:len(data)] == data and m.raw[:len(md)] == md
            assert bytes(out) == previous_result
        return rc, d.raw[:len(data)], m.raw[:out.metadata_bytes] if not rc else md, out

    def compare(data, md, delta, name):
        nonlocal positive
        expected = oracle.relay_buffer(data, md, delta, active_profile=oracle.SUPPORTED_PROFILE)
        rc, actual_data, actual_md, out = invoke(data, md, delta)
        assert rc == 0, (name, rc)
        assert actual_data == expected.data and actual_md == expected.metadata, name
        assert out.timestamp_bits == expected.timestamp_us & 0xFFFFFFFFFFFFFFFF
        assert out.wr_flags == expected.wr_flags and out.data_bytes == len(data)
        assert out.wrapper_count == expected.report["wrapper_count"]
        assert out.changed_wrapper_count == len(expected.report["changed_header_offsets"])
        assert out.secondary_zero_count == expected.report["secondary_zero_ttp_sentinels_preserved"]
        if args.native:
            folder = args.artifacts / name
            folder.mkdir()
            (folder / "data.bin").write_bytes(data)
            (folder / "metadata.bin").write_bytes(md)
            result = subprocess.run([str(args.native.resolve()), "--data", str(folder / "data.bin"),
                                     "--metadata", str(folder / "metadata.bin"), "--delta-us", str(delta),
                                     "--profile", oracle.SUPPORTED_PROFILE, "--output-dir", str(folder / "result")],
                                    text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr
            assert (folder / "result/wr-data.bin").read_bytes() == actual_data
            assert (folder / "result/wr-metadata.bin").read_bytes() == actual_md
            report = json.loads((folder / "result/relay-report.json").read_text())
            assert int(report["timestamp_bits"]) == out.timestamp_bits and report["wr_flags"] == out.wr_flags
        positive += 1

    for kind in range(9):
        for role in (0, 0x80, 0xA0, 0xC0):
            for delta in (0, 50000, 150000):
                data = packet(kind, role, 0 if role == 0xC0 else 24625)
                compare(data, metadata(data), delta, f"type-{kind}-role-{role}-delta-{delta}")
    pair = packet() + packet(role=0xC0, units=0)
    for timestamp in (0, 0xFFFFFFFF, 0x100000000, (1 << 63) - 1, -(1 << 63), -1):
        for mode in ("physical", "offset"):
            # The transform is independent of mapping mode. Exercise both sets
            # from the existing native comparison; no command is sent here.
            compare(pair, metadata(pair, timestamp), 150000, f"timestamp-{timestamp}-{mode}")
    compare(pair, metadata(pair, bt=False), 150000, "frame-info-only")

    cases = [
        (b"", b"", 0), (pair[:-1], metadata(pair), 150000),
        (packet(role=0xC0, units=1), metadata(packet()), 150000),
        (pair, metadata(pair) + record(0x0A001234, bytes(4)), 150000),
        (pair, metadata(pair) + record(0x0A00100C, bytes(4)), 150000),
        (pair, metadata(pair)[:-1], 150000),
        (pair, metadata(pair), -1), (pair, metadata(pair), 7),
        (pair, metadata(pair), 524288), (pair, metadata(pair), (1 << 64) - 8),
    ]
    for flag in (0x02, 0x40, 0x80):
        md = bytearray(metadata(pair)); struct.pack_into("<I", md, 4, flag)
        cases.append((pair, bytes(md), 150000))
    # uint64 C ABI cannot represent Python's 1<<64; replace that oracle-only
    # rejection with explicit profile and capacity boundary rejections.
    for data, md, delta in cases:
        assert invoke(data, md, delta)[0] != 0
        negative += 1
    assert invoke(pair, metadata(pair), 150000, profile=2)[0] != 0
    assert invoke(pair, metadata(pair), 150000, capacity=len(pair)-1)[0] != 0
    huge_md = metadata(pair) + bytes(65537)
    assert invoke(pair, huge_md, 150000)[0] != 0
    negative += 3

    rng = random.Random(0x53485A)
    for index in range(30000):
        data, md = bytearray(pair), bytearray(metadata(pair))
        target = data if index % 2 else md
        position = rng.randrange(len(target))
        target[position] ^= 1 << rng.randrange(8)
        if index % 7 == 0:
            target[:] = target[:rng.randrange(len(target))]
        data, md = bytes(data), bytes(md)
        try:
            expected = oracle.relay_buffer(data, md, 150000, active_profile=oracle.SUPPORTED_PROFILE)
        except oracle.RelayError:
            assert invoke(data, md, 150000)[0] != 0
        else:
            rc, d, m, out = invoke(data, md, 150000)
            assert rc == 0 and d == expected.data and m == expected.metadata
            assert out.timestamp_bits == expected.timestamp_us & 0xFFFFFFFFFFFFFFFF
        mutations += 1
    report = {"schema": 1, "synthetic_positive_cases": positive, "synthetic_rejection_cases": negative,
              "python_oracle_mutation_comparisons": mutations, "input_mutation_on_rejection": False,
              "python_oracle_sha256": hashlib.sha256(args.oracle.read_bytes()).hexdigest(),
              "c_library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest(),
              "native_cpp_comparison": bool(args.native), "live_graph_or_receiver_test": False}
    (args.artifacts / "comparison-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
