#!/usr/bin/env python3
"""Offline boundary/byte-transparency checks for the proposed encoded relay."""
import argparse
import json
from pathlib import Path
import struct

import encoded_ttp_relay as relay


def record(mid, payload, offset=0, flags=0x21, low=0, high=0):
    return struct.pack("<6I", mid, flags, offset, low, high, len(payload)) + payload


def metadata_for(data, timestamp=-0x123456789):
    timestamp &= 0xFFFFFFFFFFFFFFFF
    md = record(relay.MD_PCM_FRAME_LENGTH, struct.pack("<I", 1440))
    md += record(relay.MD_PCM_MEDIA_FORMAT, struct.pack("<IHH16H", 48000, 24, 2, 1, 2, *([0] * 14)))
    md += record(relay.MD_ZERO_PADDING, struct.pack("<I", 0))
    md += record(relay.MD_FRAME_INFO, struct.pack("<4I", len(data), timestamp & 0xFFFFFFFF,
                                                timestamp >> 32, 0x80000000), flags=0)
    return md


def packet(ttp=24625, packet_type=1, flags=0xA0):
    return struct.pack("<H6B", ttp, 120, packet_type, flags, 0, 0, 0xAE) + bytes(
        (i * 17 + 3) % 256 for i in range(relay.R3_PAYLOAD_BYTES[packet_type]))


def rejected(fn):
    try:
        fn()
    except relay.RelayError:
        return
    raise AssertionError("invalid input was accepted")


def check_synthetic():
    checked_math = 0
    for delta_us in (0, 50000, 100000, 150000):
        for before in range(65536):
            expected = before * 8 + delta_us
            if expected <= 524280:
                after = relay.ttp_with_delta(before, delta_us)
                assert after * 8 == expected
            else:
                rejected(lambda: relay.ttp_with_delta(before, delta_us))
            checked_math += 1
    for delta in (-1, 1, 7, 9, True, 1.5, 524288, 1 << 64):
        rejected(lambda: relay.ttp_with_delta(0, delta))
    packet_format_cases = 0
    for packet_type in range(9):
        for role in (0, 0x80, 0xA0, 0xC0):
            original = packet(ttp=0 if role == 0xC0 else 24625, packet_type=packet_type, flags=role)
            out = relay.relay_buffer(original, metadata_for(original), 150000, active_profile=relay.SUPPORTED_PROFILE)
            assert out.data[2:] == original[2:]
            assert int.from_bytes(out.data[:2], "little") == (0 if role == 0xC0 else 43375)
            packet_format_cases += 1

    # Two serialized Split-TX wrappers can share one encoder output frame.
    data = packet(flags=0xA0) + packet(ttp=0, flags=0xC0)
    md = metadata_for(data)
    result = relay.relay_buffer(data, md, 150000, active_profile=relay.SUPPORTED_PROFILE)
    assert result.timestamp_us == -0x123456789
    assert result.wr_flags == 0x80000010
    assert result.report["wrapper_ttp_us_before"] == [197000, 0]
    assert result.report["wrapper_ttp_us_after"] == [347000, 0]
    assert result.report["changed_header_offsets"] == [0]
    assert result.data[2:] == data[2:]
    original_md = relay.parse_metadata(md, len(data))
    assert result.metadata == b"".join(m.wire for m in original_md if m.metadata_id != relay.MD_FRAME_INFO)
    assert [m.metadata_id for m in relay.parse_metadata(result.metadata, len(data))] == [
        relay.MD_PCM_FRAME_LENGTH, relay.MD_PCM_MEDIA_FORMAT, relay.MD_ZERO_PADDING]
    unchanged = relay.relay_buffer(data, md, 0, active_profile=relay.SUPPORTED_PROFILE)
    assert unchanged.data == data
    # Never pass a plausible legacy/header variant by scanning past it.
    for position, value in ((2, 30), (3, 9), (4, 0x7F), (5, 1), (6, 1), (7, 0)):
        damaged = bytearray(data)
        damaged[position] = value
        rejected(lambda: relay.relay_buffer(bytes(damaged), md, 150000, active_profile=relay.SUPPORTED_PROFILE))
    for bad in (b"", b"junk" + data, data[:-1], data + b"\0"):
        rejected(lambda: relay.relay_buffer(bad, metadata_for(bad), 150000, active_profile=relay.SUPPORTED_PROFILE))
    # A later timing-bearing header overflow rejects the complete buffer.
    overflow = packet(ttp=24625, flags=0) + packet(ttp=50000, flags=0)
    snapshot = bytes(overflow)
    rejected(lambda: relay.relay_buffer(overflow, metadata_for(overflow), 150000, active_profile=relay.SUPPORTED_PROFILE))
    assert overflow == snapshot
    for ambiguous in (packet(ttp=1, flags=0xC0), packet(ttp=0, flags=0xA0)):
        rejected(lambda: relay.relay_buffer(ambiguous, metadata_for(ambiguous), 150000, active_profile=relay.SUPPORTED_PROFILE))
    # Nontruncated valid-looking MD cannot conceal bad ownership or sizes.
    for bad_md in (
        md[:-1], md + b"\0", b"",
        record(relay.MD_PCM_FRAME_LENGTH, b"x"),
        record(relay.MD_PCM_FRAME_LENGTH, b"1234", flags=0x40),
        record(relay.MD_PCM_FRAME_LENGTH, b"1234", flags=0x80),
        record(relay.MD_PCM_FRAME_LENGTH, b"1234", flags=0x02),
        record(relay.MD_PCM_FRAME_LENGTH, b"1234", low=1),
        record(relay.MD_PCM_FRAME_LENGTH, b"1234", offset=len(data) + 1),
        record(relay.MD_PCM_MEDIA_FORMAT, bytes(36)),
        md + original_md[-1].wire,
        md + record(0x0A001234, bytes(4)),
        md + record(0x0A00100C, bytes(4)),
    ):
        rejected(lambda: relay.relay_buffer(data, bad_md, 150000, active_profile=relay.SUPPORTED_PROFILE))
    for unproved in ("legacy-hq", "r3-hq", "r3-lossless", "r3-ll"):
        rejected(lambda: relay.relay_buffer(data, md, 150000, active_profile=unproved))

    # Signed 64-bit session timestamps must survive beyond 32-bit rollover.
    for timestamp in (0, 0xFFFFFFFF, 0x100000000, (1 << 63) - 1, -(1 << 63), -1):
        r = relay.relay_buffer(data, metadata_for(data, timestamp), 0, active_profile=relay.SUPPORTED_PROFILE)
        assert r.timestamp_us == timestamp
        body = relay.build_wr_data_command(r, data_address=0x123456780, data_map_handle=7,
                                          metadata_address=0x234567880, metadata_map_handle=8)
        words = struct.unpack("<11I", body)
        assert len(body) == 44 and words[:4] == (0x23456780, 1, 7, len(data))
        assert (words[5] << 32 | words[4]) == timestamp & 0xFFFFFFFFFFFFFFFF
        assert words[6:] == (0x80000010, 0x34567880, 2, 8, len(r.metadata))
    for address, handle in ((0, 1), (65, 1), (1 << 36, 1), ((1 << 36) - 64, 1), (64, 0), (64, 1 << 32)):
        rejected(lambda: relay.build_wr_data_command(result, data_address=address, data_map_handle=handle,
                                                     metadata_address=128, metadata_map_handle=1))
    return {"exhaustive_ttp_cases": checked_math, "synthetic_r3_packet_type_role_cases": packet_format_cases,
            "synthetic_split_tx_delta_us": 150000,
            "synthetic_split_tx_before_us": [197000, 0], "synthetic_split_tx_after_us": [347000, 0],
            "secondary_zero_ttp_sentinel_preserved": True,
            "metadata_offsets_tokens_flags_preserved": True,
            "unknown_and_eos_metadata_rejected": True,
            "unproved_legacy_lossless_ll_profiles_rejected": True,
            "session_timestamp_signed_64bit_boundaries_checked": True,
            "overflow_or_malformed_buffer_rejected_atomically": True,
            "runtime_graph_or_receiver_test": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    report = {"synthetic": check_synthetic()}
    args.artifacts.mkdir(parents=True, exist_ok=True)
    (args.artifacts / "test-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
