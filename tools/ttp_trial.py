#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build opt-in +50/+100 ms HQ/Lossless DSP experiments, without device writes."""

import argparse
import json
import shutil
from pathlib import Path

from shizuka.ttp import inspect_stock, validate_control_plan, write_candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser(
        "inspect", help="verify the exact stock image and report compiled defaults"
    )
    inspect.add_argument("--input", type=Path, required=True)
    build = sub.add_parser("build", help="create a candidate; never install or restart anything")
    build.add_argument("--input", type=Path, required=True)
    tuning = build.add_mutually_exclusive_group(required=True)
    tuning.add_argument("--delta-ms", type=int, choices=(0, 50, 100))
    tuning.add_argument(
        "--plan", type=Path, help="JSON plan from config.sh tuning plan or codec_control.py"
    )
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--llvm-mc", default=shutil.which("llvm-mc"))
    build.add_argument("--linker", default=shutil.which("ld.lld"))
    args = parser.parse_args()
    try:
        if args.command == "inspect":
            result = inspect_stock(args.input.read_bytes())
        else:
            plan = None
            if args.plan:
                if args.plan.stat().st_size > 32768:
                    raise ValueError("Codec-control plan is too large")
                plan = json.loads(args.plan.read_text())
                args.delta_ms = validate_control_plan(plan)
            if args.delta_ms and (not args.llvm_mc or not args.linker):
                parser.error("Set --llvm-mc and --linker to LLVM tools with Hexagon support")
            result = write_candidate(
                args.input, args.output, args.delta_ms, args.llvm_mc, args.linker, plan
            )
        print(json.dumps(result, indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, f"TTP experiment refused: {error}\n")


if __name__ == "__main__":
    main()
