#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host entry point for shared Android control planning and offline DSP log replay."""

import argparse
import base64
import json
import math
import re
import subprocess
import tempfile
from pathlib import Path

from shizuka.java import java_tools
from shizuka.project import ANDROID

CORE = (
    "ClassicQhsPolicy",
    "ExtensionConfig",
    "CodecPresentation",
    "ControlJson",
    "CodecTuning",
    "DspLogDecoder",
    "DspReplay",
    "CodecControlTool",
)


def records(path, format_name):
    if path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("Replay input exceeds 32 MiB")
    pending, time = [], None
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if format_name == "native":
                item = json.loads(line)
                if item.get("type") == "dsp_log":
                    yield float(item["monotonic_s"]), item["text"]
            else:
                match = re.match(r"^(\d+\.\d+)\s+(.+)$", line)
                if match:
                    if time is not None:
                        yield time, "".join(pending)
                    time, pending = float(match[1]), [match[2] + "\n"]
                elif time is not None:
                    pending.append(line)
        if format_name == "ring" and time is not None:
            yield time, "".join(pending)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("capabilities")
    plan = sub.add_parser("plan")
    plan.add_argument("--config", type=Path, required=True)
    plan.add_argument("--profile", default="samsung-sm-s931z-dzif")
    plan.add_argument("--codec", required=True)
    plan.add_argument("--mode", required=True)
    plan.add_argument("--rate", type=int, required=True)
    plan.add_argument("--bits", type=int, required=True)
    replay = sub.add_parser("replay", help="offline only; never publishes current telemetry")
    replay.add_argument("input", type=Path)
    replay.add_argument("--format", choices=("native", "ring"), default="native")
    args = parser.parse_args()
    java, compiler = java_tools()
    with tempfile.TemporaryDirectory(prefix="shizuka-controls-") as directory:
        subprocess.run(
            [
                *compiler,
                "--release",
                "8",
                "-d",
                directory,
                *[str(ANDROID / (name + ".java")) for name in CORE],
            ],
            check=True,
        )
        if args.command == "replay":
            process = subprocess.Popen(
                [java, "-cp", directory, "DspReplay"], stdin=subprocess.PIPE, text=True
            )
            try:
                for time, text in records(args.input, args.format):
                    if (
                        not math.isfinite(time)
                        or time < 0
                        or not isinstance(text, str)
                        or len(text) > 16384
                    ):
                        raise ValueError("Invalid replay observation")
                    encoded = base64.b64encode(text.encode()).decode()
                    process.stdin.write(f"{time}\t{encoded}\n")
                process.stdin.close()
                if process.wait() != 0:
                    raise ValueError("DSP replay failed")
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
        else:
            command = [java, "-cp", directory, "CodecControlTool", args.command]
            if args.command == "plan":
                command += [
                    str(args.config),
                    args.profile,
                    args.codec,
                    args.mode,
                    str(args.rate),
                    str(args.bits),
                ]
            subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
