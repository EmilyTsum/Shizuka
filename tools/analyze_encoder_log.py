# SPDX-License-Identifier: GPL-2.0-only
"""Summarize producer-ring observations, not radio packet delivery or audible timing."""

import argparse
import json
import re
from pathlib import Path

RECORD = re.compile(
    r"^(\d+\.\d+) [^\n]*SPF APTX_ADAPTIVE3_ENC: \n"
    r"Packet- #(\d+)\nHQ State- (\S+)\nSize- (\d+)\n"
    r"Channels- (\d+)\nInterval- ([\d.]+)",
    re.M,
)


def analyze(log, markers=""):
    packets = [
        dict(time=float(m[1]), number=int(m[2]), state=m[3], interval_ms=float(m[6]))
        for m in RECORD.finditer(log)
    ]
    if not packets:
        raise ValueError("No complete timestamped encoder packet observations")
    pairs = list(zip(packets, packets[1:]))
    if any(b["time"] < a["time"] for a, b in pairs):
        raise ValueError("Observation clock moves backwards; do not merge sessions")
    transitions = [b for a, b in pairs if a["state"] != b["state"]]
    clicks = []
    for raw in re.findall(r"^(\d+\.\d+) KEY_VOLUMEDOWN 1$", markers, re.M):
        time = float(raw)
        item = {"marker_s": time, "covered": packets[0]["time"] <= time <= packets[-1]["time"]}
        if item["covered"]:
            closest = min(packets, key=lambda p: abs(p["time"] - time))
            current = next(p for p in reversed(packets) if p["time"] <= time)
            window = [p for p in packets if abs(p["time"] - time) <= 1]
            window_pairs = list(zip(window, window[1:]))
            prior = [p for p in transitions if p["time"] <= time]
            since = prior[-1]["time"] if prior else packets[0]["time"]
            item.update(
                nearest_packet=closest["number"],
                state=current["state"],
                state_observed_for_s=round(time - since, 6),
                transitions_within_1s=sum(abs(p["time"] - time) <= 1 for p in transitions),
                nonconsecutive_packet_numbers_within_1s=sum(
                    b["number"] != a["number"] + 1 for a, b in window_pairs
                ),
                max_observed_gap_within_1s_ms=round(
                    max((b["time"] - a["time"] for a, b in window_pairs), default=0) * 1000, 3
                ),
            )
        clicks.append(item)
    return dict(
        packet_observations=len(packets),
        observed_span_s=round(packets[-1]["time"] - packets[0]["time"], 6),
        nonconsecutive_packet_numbers=sum(b["number"] != a["number"] + 1 for a, b in pairs),
        transitions=transitions,
        markers=clicks,
        limitations=[
            "Times are host observations, not DSP generation or receiver playout times.",
            "Button presses include human reaction time and can change audio gain.",
            "Consecutive packet numbers do not establish valid encoded content or radio delivery.",
            "Counter resets, wrap, replay or lost log records can also cause discontinuities.",
        ],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--markers", type=Path)
    args = parser.parse_args()
    print(
        json.dumps(
            analyze(args.log.read_text(), args.markers.read_text() if args.markers else ""),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
