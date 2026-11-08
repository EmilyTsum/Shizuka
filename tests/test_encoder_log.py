# SPDX-License-Identifier: GPL-2.0-only
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_encoder_log import analyze


def packet(time, number, level=4):
    return (
        f"{time:.6f} thread: ADSP: SPF APTX_ADAPTIVE3_ENC: \n"
        f"Packet- #{number}\nHQ State- STATE_ENCLEVEL_{level}\n"
        "Size- 656\nChannels- 2\nInterval- 30.0\n"
    )


class EncoderLogTest(unittest.TestCase):
    def test_click_without_transition(self):
        result = analyze(
            packet(10, 1) + packet(10.03, 2) + packet(10.06, 3),
            "10.040000 KEY_VOLUMEDOWN 1\n10.050000 KEY_VOLUMEDOWN 0\n",
        )
        self.assertEqual(len(result["markers"]), 1)
        marker = result["markers"][0]
        self.assertTrue(marker["covered"])
        self.assertEqual(marker["transitions_within_1s"], 0)
        self.assertEqual(marker["nearest_packet"], 2)
        self.assertEqual(marker["max_observed_gap_within_1s_ms"], 30)

    def test_discontinuity_and_outside_marker(self):
        result = analyze(packet(10, 1) + packet(10.03, 5, 3), "20.000000 KEY_VOLUMEDOWN 1\n")
        self.assertEqual(result["nonconsecutive_packet_numbers"], 1)
        self.assertEqual(len(result["transitions"]), 1)
        self.assertFalse(result["markers"][0]["covered"])
        self.assertNotIn("state", result["markers"][0])

    def test_reject_empty_or_mixed_clock(self):
        for log in ["Packet- #1\n", packet(10, 1) + packet(9, 2)]:
            with self.assertRaises(ValueError):
                analyze(log)


if __name__ == "__main__":
    unittest.main()
