#!/usr/bin/env python3
"""Synthetic wire checks only; no device, GPR, allocator or private inputs."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
import endpoint_wire_protocol as wire


class EndpointWireTest(unittest.TestCase):
    def setUp(self):
        self.data = wire.SubmittedBuffer(0, 0x12345678, 4096)
        self.md = wire.SubmittedBuffer(4096, 0x12345678, 4096)
        self.rd = [0, 0, 0, self.data.map_handle, 664,
                   7, 1, 0x80000000, 1,
                   0, 4096, 0, self.md.map_handle, 160]
        self.wr = [0, 0, self.data.map_handle, 0,
                   4096, 0, self.md.map_handle, 0]

    def rd_done(self, words=None):
        return wire.parse_rd_done(struct.pack("<14I", *(words or self.rd)), self.data, self.md)

    def wr_done(self, words=None):
        return wire.parse_wr_done(struct.pack("<8I", *(words or self.wr)), self.data, self.md)

    def test_map_matches_explicit_twenty_byte_contract(self):
        flags, body = wire.map_request_for_osal_fd(7, 16384)
        self.assertEqual(flags, 1)
        self.assertEqual(body.hex(), "0300010004000000070000000000000000400000")
        self.assertEqual(struct.unpack("<HH4I", body), (3, 1, 4, 7, 0, 16384))
        self.assertEqual(wire.unmap_request(0x12345678), bytes.fromhex("78563412"))
        for fd in (-1, 2**31, True, 1.5):
            with self.subTest(fd=fd), self.assertRaises(wire.Invalid):
                wire.map_request_for_osal_fd(fd, 16384)
        for size in (0, 4095, 4097, 2**32, True):
            with self.subTest(size=size), self.assertRaises(wire.Invalid):
                wire.map_request_for_osal_fd(7, size)
        for handle in (0, -1, 2**32, True):
            with self.subTest(handle=handle), self.assertRaises(wire.Invalid):
                wire.unmap_request(handle)

    def test_valid_done_and_signed_timestamp_boundaries(self):
        self.assertEqual(self.rd_done()["data_bytes"], 664)
        self.assertEqual(self.rd_done()["metadata_bytes"], 160)
        self.assertEqual(self.rd_done()["timestamp_us"], 0x100000007)
        self.assertTrue(self.wr_done()["data_consumed"])
        for timestamp in (0, 2**32 - 1, 2**32, 2**63 - 1, -2**63, -1):
            words = self.rd.copy()
            unsigned = timestamp & (2**64 - 1)
            words[5:7] = [unsigned & 0xFFFFFFFF, unsigned >> 32]
            with self.subTest(timestamp=timestamp):
                self.assertEqual(self.rd_done(words)["timestamp_us"], timestamp)
        words = self.rd.copy()
        words[7] = 0
        self.assertFalse(self.rd_done(words)["timestamp_valid"])

    def test_exact_sizes_not_prefix_parsing(self):
        for size in (0, 1, 32, 52, 55, 57, 60):
            with self.subTest(rd_size=size), self.assertRaises(wire.Invalid):
                wire.parse_rd_done(bytes(size), self.data, self.md)
        for size in (0, 1, 28, 31, 33, 36, 56):
            with self.subTest(wr_size=size), self.assertRaises(wire.Invalid):
                wire.parse_wr_done(bytes(size), self.data, self.md)

    def test_rd_status_address_handle_capacity_flags_and_frame_count(self):
        # Every field with ownership/status semantics gets an independent
        # malformed packet. Timestamp words are allowed to carry all bits.
        bad_values = {0: (1, 0xFFFFFFFF), 1: (64,), 2: (1,), 3: (0, 1),
                      4: (0, 4097), 7: (1, 0x40000000, 0x80000001),
                      8: (0, 2, 0xFFFFFFFF), 9: (1, 0xFFFFFFFF),
                      10: (0, 4160), 11: (1,), 12: (0, 1), 13: (0, 4097)}
        for index, values in bad_values.items():
            for value in values:
                words = self.rd.copy()
                words[index] = value
                with self.subTest(field=index, value=value), self.assertRaises(wire.Invalid):
                    self.rd_done(words)
        words = self.rd.copy()
        words[4] = words[13] = 4096
        self.assertEqual(self.rd_done(words)["metadata_bytes"], 4096)

    def test_wr_both_ownership_records_and_statuses(self):
        for index, value in enumerate((64, 1, 1, 1, 0, 1, 1, 1)):
            words = self.wr.copy()
            words[index] = value
            with self.subTest(field=index), self.assertRaises(wire.Invalid):
                self.wr_done(words)

    def test_offset_is_full_64_bit(self):
        # These are payload-serializer checks, not permission to map a region
        # over 4 GiB. The live mapping owner must separately check its size.
        data = wire.SubmittedBuffer(0x1234567800000040, 11, 1024)
        md = wire.SubmittedBuffer(0x1234567800001040, 11, 1024)
        payload = struct.pack("<14I", 0, 0x40, 0x12345678, 11, 664,
                              0, 0, 0, 1, 0, 0x1040, 0x12345678, 11, 160)
        self.assertEqual(wire.parse_rd_done(payload, data, md)["data_bytes"], 664)
        with self.assertRaises(wire.Invalid):
            wire.parse_rd_done(payload, self.data, self.md)


if __name__ == "__main__":
    unittest.main(verbosity=2)
