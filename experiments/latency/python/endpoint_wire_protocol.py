#!/usr/bin/env python3
"""Offline endpoint wire-format validators. No GPR/device/allocator calls."""
from dataclasses import dataclass
import struct


class Invalid(ValueError):
    pass


@dataclass(frozen=True)
class SubmittedBuffer:
    address_or_offset: int
    map_handle: int
    capacity: int


def map_request_for_osal_fd(fd: int, size: int) -> tuple[int, bytes]:
    """Return GPR client_data and a 20B APM MAP body; do not send it.

    Requires installed OSAL layout-v1 offset/physical memory semantics. The
    sending process must own this fd and its audio-ion registration. The fd is
    NOT the map handle, a host pointer, or a physical address.
    """
    if type(fd) is not int or not 0 <= fd <= 0x7FFFFFFF:
        raise Invalid("fd must be an open nonnegative process-local descriptor")
    if type(size) is not int or size < 4096 or size > 0xFFFFFFFF or size % 4096:
        raise Invalid("MAP requires 4KiB-aligned positive uint32 size")
    # Physical memory / cached in SPF / offset addressing; normal GSL flags=0.
    return 1, struct.pack("<HH4I", 3, 1, 4, fd, 0, size)


def unmap_request(map_handle: int) -> bytes:
    if type(map_handle) is not int or not 0 < map_handle <= 0xFFFFFFFF:
        raise Invalid("requires this owner's acknowledged nonzero APM handle")
    return struct.pack("<I", map_handle)


def validate_buffer(address: int, handle: int, actual: int, submitted: SubmittedBuffer):
    if address != submitted.address_or_offset or handle != submitted.map_handle:
        raise Invalid("DONE address/offset or map handle differs from submitted ownership record")
    if actual < 0 or actual > submitted.capacity:
        raise Invalid("DONE size exceeds submitted capacity")


def parse_rd_done(payload: bytes, data: SubmittedBuffer, metadata: SubmittedBuffer) -> dict:
    """56B DATA_CMD_RSP_RD_SH_MEM_EP_DATA_BUFFER_DONE_V2, opcode05001005.

    The caller must first validate GPR domains, exact RD MIID/client source
    port, packet size, current graph epoch, unique live submission token and
    DATA_OWNED_BY_RD state. This parser does not establish any of those facts.
    """
    if len(payload) != 56:
        raise Invalid("RD_DONE must be exactly 56B for out-of-band MD")
    status, low, high, handle, size, tlow, thigh, flags, frames, md_status, mlow, mhigh, mhandle, msize = struct.unpack("<14I", payload)
    validate_buffer(low | high << 32, handle, size, data)
    validate_buffer(mlow | mhigh << 32, mhandle, msize, metadata)
    if status or md_status:
        raise Invalid("RD_DONE reports data or metadata error; abort this relay epoch")
    if not size or not msize or frames != 1 or flags & ~0x80000000:
        raise Invalid("RD_DONE is not one nonempty valid reviewed frame")
    timestamp = tlow | thigh << 32
    if timestamp & 1 << 63:
        timestamp -= 1 << 64
    return {"data_bytes": size, "metadata_bytes": msize, "num_frames": frames,
            "timestamp_us": timestamp, "timestamp_valid": bool(flags & 0x80000000)}


def parse_wr_done(payload: bytes, data: SubmittedBuffer, metadata: SubmittedBuffer) -> dict:
    """32B DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2, opcode05001004."""
    if len(payload) != 32:
        raise Invalid("WR_DONE must be exactly 32B")
    low, high, handle, status, mlow, mhigh, mhandle, md_status = struct.unpack("<8I", payload)
    validate_buffer(low | high << 32, handle, 0, data)
    validate_buffer(mlow | mhigh << 32, mhandle, 0, metadata)
    if status or md_status:
        raise Invalid("WR_DONE reports data or metadata error; abort this relay epoch")
    return {"data_consumed": True, "metadata_consumed": True}


def _self_test():
    flags, body = map_request_for_osal_fd(7, 16384)
    assert flags == 1 and len(body) == 20 and body.hex() == "0300010004000000070000000000000000400000"
    data, md = SubmittedBuffer(0, 11, 4096), SubmittedBuffer(4096, 11, 4096)
    good_rd = struct.pack("<14I", 0, 0, 0, 11, 664, 7, 1, 0x80000000, 1, 0, 4096, 0, 11, 160)
    assert parse_rd_done(good_rd, data, md)["timestamp_us"] == 0x100000007
    good_wr = struct.pack("<8I", 0, 0, 11, 0, 4096, 0, 11, 0)
    assert parse_wr_done(good_wr, data, md)["data_consumed"]
    tests = [(lambda: map_request_for_osal_fd(-1, 4096)), (lambda: map_request_for_osal_fd(7, 4097)),
             (lambda: unmap_request(0)), (lambda: parse_rd_done(good_rd[:-1], data, md)),
             (lambda: parse_wr_done(good_wr + bytes(4), data, md))]
    for index in (0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13):
        bad = bytearray(good_rd)
        values = {0:1,1:64,2:1,3:12,4:4097,7:1,8:2,9:1,10:0,11:1,12:12,13:4097}
        struct.pack_into("<I", bad, index*4, values[index])
        tests.append(lambda b=bytes(bad): parse_rd_done(b, data, md))
    for test in tests:
        try:
            test()
        except Invalid:
            continue
        raise AssertionError("malformed/incorrect-ownership wire packet accepted")
    print("endpoint wire self-test passed: MAP layout, offset0, RD/WR DONE ownership, timestamp rollover, malformed rejection")


if __name__ == "__main__":
    _self_test()
