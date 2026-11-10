#!/usr/bin/env python3
"""Offline R3 encoded-TTP relay prototype; never opens or changes a DSP graph.

Input is one successful OLC RD buffer and its exact metadata byte count.
Output is one WR buffer. This is deliberately narrower than a live relay:
one encoder-frame-info record must cover the complete data buffer; unproved
tracking, extensions, unaligned MD, and other codec versions fail closed.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct

MD_FRAME_INFO = 0x0A001052
MD_PCM_FRAME_LENGTH = 0x0A001037
MD_PCM_MEDIA_FORMAT = 0x0A001038
MD_ZERO_PADDING = 0x0A001039
BT_MD_SIZES = {MD_PCM_FRAME_LENGTH: 4, MD_PCM_MEDIA_FORMAT: 40, MD_ZERO_PADDING: 4}
REVIEWED_METADATA_IDS = {*BT_MD_SIZES, MD_FRAME_INFO}
SUPPORTED_PROFILE = "stock-r3-hq-44100"
R3_PAYLOAD_BYTES = (348, 656, 140, 152, 560, 760, 960, 348, 980)
R3_FLAGS = {0, 0x80, 0xA0, 0xC0}
R3_MARKERS = {0xAD, 0xAE, 0xAF}
MD_HEADER = struct.Struct("<6I")
FRAME_INFO = struct.Struct("<4I")
TTP_QUANTUM_US = 8
MAX_TTP_UNITS = 0xFFFF
MAX_TTP_US = MAX_TTP_UNITS * TTP_QUANTUM_US
TIMESTAMP_VALID = 1 << 31
END_OF_FRAME = 1 << 4


class RelayError(ValueError):
    """The complete RD buffer must be rejected without sending partial output."""


@dataclass(frozen=True)
class Metadata:
    metadata_id: int
    flags: int
    offset: int
    token_lsw: int
    token_msw: int
    payload: bytes
    wire: bytes


@dataclass(frozen=True)
class Wrapper:
    offset: int
    size: int
    packet_type: int
    flags: int
    nominal_interval_field_ms: int
    old_ttp_units: int


@dataclass(frozen=True)
class RelayResult:
    data: bytes
    metadata: bytes
    timestamp_us: int
    wr_flags: int
    report: dict


def parse_metadata(data: bytes, encoded_size: int) -> list[Metadata]:
    """Parse the actual old-firmware 24B+payload layout, with strict limits.

    Public metadata_api.h permits 4B padding and bit6 extensions; this phone's
    WR loop advances by 24+payload_size without rounding. Only naturally 4B
    sized records are accepted here. Tracking needs OLC acknowledgement and
    token lifetime handling, so it must not silently pass through this draft.
    """
    records = []
    cursor = 0
    while cursor < len(data):
        if len(data) - cursor < MD_HEADER.size:
            raise RelayError(f"truncated metadata header at {cursor}")
        mid, flags, offset, token_lsw, token_msw, size = MD_HEADER.unpack_from(data, cursor)
        if mid not in REVIEWED_METADATA_IDS:
            raise RelayError(f"metadata {mid:08x}: ID/lifetime is outside reviewed allowlist")
        if flags & ~0x7F:
            raise RelayError(f"metadata {mid:08x}: reserved flags")
        if flags & 0x40:
            raise RelayError(f"metadata {mid:08x}: extension ownership not implemented")
        if flags & 0x0E or token_lsw or token_msw:
            raise RelayError(f"metadata {mid:08x}: tracking ownership not implemented")
        if size % 4:
            raise RelayError(f"metadata {mid:08x}: old WR requires naturally 4B-sized payload")
        if size > len(data) - cursor - MD_HEADER.size:
            raise RelayError(f"metadata {mid:08x}: payload exceeds available metadata")
        if offset > encoded_size:
            raise RelayError(f"metadata {mid:08x}: raw-compressed byte offset exceeds data")
        end = cursor + MD_HEADER.size + size
        payload = data[cursor + MD_HEADER.size:end]
        if mid in BT_MD_SIZES and size != BT_MD_SIZES[mid]:
            raise RelayError(f"metadata {mid:08x}: wrong Bluetooth payload size")
        if mid == MD_FRAME_INFO and size != FRAME_INFO.size:
            raise RelayError("encoder frame-info must have a 16-byte payload")
        # Preserve the entire reviewed original record; do not re-encode it.
        records.append(Metadata(mid, flags, offset, token_lsw, token_msw, payload, data[cursor:end]))
        cursor = end
    return records


def parse_r3_wrappers(data: bytes) -> list[Wrapper]:
    """Parse contiguous wrappers at proven boundaries; never scan or resync.

    Length table is from the stock signed wrapper's VA0x210d8. The 8B header
    is emitted after encoder execution in R3 process VA0xe494..0xe730.
    This format must be positively selected by the live graph owner; a
    plausible header alone must not be used to identify the active codec.
    """
    if not data:
        raise RelayError("empty encoded buffer")
    wrappers = []
    cursor = 0
    while cursor < len(data):
        if len(data) - cursor < 8:
            raise RelayError(f"truncated R3 wrapper header at {cursor}")
        ttp, period4, packet_type, flags, reserved0, reserved1, marker = struct.unpack_from("<H6B", data, cursor)
        if packet_type >= len(R3_PAYLOAD_BYTES):
            raise RelayError(f"unknown R3 packet type {packet_type} at {cursor}")
        if flags not in R3_FLAGS or reserved0 or reserved1 or marker not in R3_MARKERS:
            raise RelayError(f"unsupported R3 wrapper header at {cursor}")
        if period4 == 0 or period4 % 4:
            raise RelayError(f"unsupported R3 interval field at {cursor}")
        size = 8 + R3_PAYLOAD_BYTES[packet_type]
        if size > len(data) - cursor:
            raise RelayError(f"truncated R3 packet at {cursor}")
        wrappers.append(Wrapper(cursor, size, packet_type, flags, period4 // 4, ttp))
        cursor += size
    return wrappers


def ttp_with_delta(old_units: int, delta_us: int) -> int:
    if type(delta_us) is not int or delta_us < 0:
        raise RelayError("delta_us must be a nonnegative integer")
    if delta_us % TTP_QUANTUM_US:
        raise RelayError("delta_us must be an exact multiple of 8 microseconds")
    if not 0 <= old_units <= MAX_TTP_UNITS:
        raise RelayError("old TTP is outside the unsigned 16-bit range")
    # Python integers are unbounded. A native port must use uint64_t before
    # addition and range check, then narrow exactly once. Never wrap or clamp.
    updated = old_units + delta_us // TTP_QUANTUM_US
    if updated > MAX_TTP_UNITS:
        raise RelayError(f"TTP overflow: {old_units * 8}+{delta_us} exceeds {MAX_TTP_US} us")
    return updated


def relay_buffer(data: bytes, metadata: bytes, delta_us: int, *, active_profile: str) -> RelayResult:
    """Validate everything before changing any data; no timestamp/PCM edits.

    A running relay must snapshot delta_us once per returned RD frame, not
    between split-channel wrappers. Independent HQ/LL settings are selected
    by the graph owner; this function requires that profile explicitly.
    """
    if active_profile != SUPPORTED_PROFILE:
        raise RelayError("only observed stock R3 / 44.1 kHz / HQ format is reviewed")
    # Validate the delta even if encoded input later fails.
    ttp_with_delta(0, delta_us)
    records = parse_metadata(metadata, len(data))
    frame_records = [m for m in records if m.metadata_id == MD_FRAME_INFO]
    if len(frame_records) != 1:
        raise RelayError("draft relay requires exactly one encoder frame-info record")
    frame = frame_records[0]
    frame_size, ts_low, ts_high, ts_flags = FRAME_INFO.unpack(frame.payload)
    if frame.offset != 0 or frame_size != len(data):
        raise RelayError("encoder frame-info must cover the entire RD buffer")
    if ts_flags & ~TIMESTAMP_VALID:
        raise RelayError("reserved encoder frame-info timestamp flags")
    wrappers = parse_r3_wrappers(data)
    updated_units = []
    for wrapper in wrappers:
        if wrapper.flags == 0xC0:
            # Stock R3 explicitly initializes secondary headers with zero TTP
            # at VA0xc2ec/0xdaf0; all archived secondary wrappers are zero.
            # calcTTPAdj is written only to the primary/combined/mono header.
            # Preserve that sentinel, rather than inventing secondary timing.
            if wrapper.old_ttp_units:
                raise RelayError("unexpected nonzero secondary TTP; receiver semantics unproved")
            updated_units.append(0)
        else:
            if delta_us and not wrapper.old_ttp_units:
                raise RelayError("zero primary TTP may be unset; cannot safely add a delta")
            updated_units.append(ttp_with_delta(wrapper.old_ttp_units, delta_us))
    # Validation is complete: all timing-bearing headers change atomically.
    output = bytearray(data)
    changed_offsets = []
    for wrapper, new_units in zip(wrappers, updated_units):
        struct.pack_into("<H", output, wrapper.offset, new_units)
        if new_units != wrapper.old_ttp_units:
            changed_offsets.append(wrapper.offset)
    # Frame info belongs to the external RD client; it is not encoder BT MD.
    wr_metadata = b"".join(m.wire for m in records if m.metadata_id != MD_FRAME_INFO)
    unsigned_ts = ts_low | (ts_high << 32)
    timestamp_us = unsigned_ts - (1 << 64) if unsigned_ts & (1 << 63) else unsigned_ts
    wr_flags = END_OF_FRAME | (ts_flags & TIMESTAMP_VALID)
    report = {
        "status": "offline-prototype-only; no DSP graph or receiver runtime validation",
        "active_profile": active_profile,
        "profile_scope_is_not_lossless_or_ll_support": True,
        "delta_us": delta_us,
        "ttp_quantum_us": TTP_QUANTUM_US,
        "maximum_serialized_ttp_us": MAX_TTP_US,
        "input_data_bytes": len(data),
        "wrapper_count": len(wrappers),
        "timing_bearing_wrapper_count": sum(w.flags != 0xC0 for w in wrappers),
        "secondary_zero_ttp_sentinels_preserved": sum(w.flags == 0xC0 for w in wrappers),
        "changed_header_offsets": changed_offsets,
        "wrapper_ttp_us_before": [w.old_ttp_units * 8 for w in wrappers],
        "wrapper_ttp_us_after": [u * 8 for u in updated_units],
        "bt_metadata_preserved": [f"0x{m.metadata_id:08X}" for m in records if m.metadata_id in BT_MD_SIZES],
        "all_forwarded_md_ids": [f"0x{m.metadata_id:08X}" for m in records if m.metadata_id != MD_FRAME_INFO],
        "consumed_client_md_ids": [f"0x{MD_FRAME_INFO:08X}"],
        "wr_timestamp_us_unchanged": timestamp_us,
        "wr_flags": f"0x{wr_flags:08X}",
        "input_sha256": hashlib.sha256(data).hexdigest(),
        "output_sha256": hashlib.sha256(output).hexdigest(),
        "encoded_body_bytes_unchanged": all(
            output[w.offset + 2:w.offset + w.size] == data[w.offset + 2:w.offset + w.size]
            for w in wrappers
        ),
        "represented_pcm_samples_modified": False,
        "session_timestamp_modified": False,
    }
    return RelayResult(bytes(output), wr_metadata, timestamp_us, wr_flags, report)


def build_wr_data_command(result: RelayResult, *, data_address: int, data_map_handle: int,
                          metadata_address: int, metadata_map_handle: int) -> bytes:
    """Serialize only the 44B DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 body.

    Addresses must refer to existing APM-mapped shared memory, with completed
    host/DSP cache synchronization. This function neither allocates nor maps
    memory and cannot prove a supplied map handle's ownership or validity.
    """
    def mapped(address: int, handle: int, size: int, label: str) -> tuple[int, int, int, int]:
        if not 0 < size <= 0xFFFFFFFF:
            raise RelayError(f"{label}: mapped buffer must have a positive uint32 size")
        if type(address) is not int or not 0 < address < (1 << 36) or address % 64:
            raise RelayError(f"{label}: expected 64-byte-aligned 32/36-bit APM-mapped address")
        if address + size > (1 << 36):
            raise RelayError(f"{label}: mapped range overflows 36-bit address space")
        if type(handle) is not int or not 0 < handle <= 0xFFFFFFFF:
            raise RelayError(f"{label}: expected an existing nonzero uint32 APM map handle")
        return address & 0xFFFFFFFF, address >> 32, handle, size

    data = mapped(data_address, data_map_handle, len(result.data), "data")
    if result.metadata:
        md = mapped(metadata_address, metadata_map_handle, len(result.metadata), "metadata")
    else:
        if metadata_address or metadata_map_handle:
            raise RelayError("empty metadata requires zero address and handle")
        md = (0, 0, 0, 0)
    timestamp = result.timestamp_us & 0xFFFFFFFFFFFFFFFF
    return struct.pack("<11I", *data, timestamp & 0xFFFFFFFF, timestamp >> 32,
                       result.wr_flags, *md)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--delta-us", required=True, type=int)
    parser.add_argument("--profile", choices=[SUPPORTED_PROFILE], required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = relay_buffer(args.data.read_bytes(), args.metadata.read_bytes(), args.delta_us,
                              active_profile=args.profile)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        paths = [args.output_dir / name for name in ("wr-data.bin", "wr-metadata.bin", "relay-report.json")]
        if any(p.exists() or p.resolve() in {args.data.resolve(), args.metadata.resolve()} for p in paths):
            raise RelayError("refusing to overwrite any input or existing result")
        # Files are written only after the complete buffer has passed validation.
        paths[0].write_bytes(result.data)
        paths[1].write_bytes(result.metadata)
        paths[2].write_text(json.dumps(result.report, indent=2) + "\n")
        print(json.dumps(result.report, indent=2))
    except (RelayError, OSError) as exc:
        parser.exit(2, f"relay rejected buffer: {exc}\n")


if __name__ == "__main__":
    main()
