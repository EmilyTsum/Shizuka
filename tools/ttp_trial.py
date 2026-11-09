#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build an opt-in DSP tuning candidate using a reviewed exact-image backend."""

import argparse
import json
import re
import shutil
from pathlib import Path

from shizuka.project import digest
from shizuka.ttp import build_candidate, inspect_stock, parameters_from_plan, resolve_backend
from shizuka.tuning import control_for, integer, load_catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser(
        "inspect", help="verify the exact stock image and report compiled defaults"
    )
    inspect.add_argument("--input", type=Path, required=True)
    inspect.add_argument("--profile", help="optional explicit runtime profile")
    build = sub.add_parser("build", help="create a candidate; never install or restart anything")
    build.add_argument("--input", type=Path, required=True)
    build.add_argument("--delta-ms", "--hq-delta-ms", type=int)
    build.add_argument("--ll-delta-ms", type=int)
    build.add_argument("--profile", help="optional explicit runtime profile")
    build.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="KEY=INTEGER",
        help="canonical tuning parameter (repeatable); domains come from capabilities",
    )
    build.add_argument(
        "--plan", type=Path, help="JSON plan from config.sh tuning plan or codec_control.py"
    )
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--llvm-mc", default=shutil.which("llvm-mc"))
    build.add_argument("--linker", default=shutil.which("ld.lld"))
    args = parser.parse_args()
    try:
        if args.command == "inspect":
            result = inspect_stock(args.input.read_bytes(), args.profile)
        else:
            plan = None
            backend = resolve_backend(profile=args.profile, stock_sha256=digest(args.input))
            direct = list(args.set)
            # Compatibility flags map into the same canonical key/value path.
            for key, value in (
                (getattr(backend, "HQ_KEY", None), args.delta_ms),
                (getattr(backend, "LL_KEY", None), args.ll_delta_ms),
            ):
                if value is not None:
                    if key is None:
                        raise ValueError("This backend requires canonical --set parameters")
                    direct.append(f"{key}={value}")
            if args.plan:
                if direct:
                    parser.error("--plan cannot be combined with direct parameters")
                if args.plan.stat().st_size > 32768:
                    raise ValueError("Codec-control plan is too large")
                plan = json.loads(args.plan.read_text())
                planned_backend, parameters = parameters_from_plan(plan)
                if backend is not planned_backend:
                    raise ValueError("Plan and input DSP backends differ")
            else:
                if not direct:
                    parser.error(
                        "Specify --plan, --set KEY=INTEGER or a compatibility increment flag"
                    )
                catalog = load_catalog()
                profiles = [
                    profile for profile in backend.PROFILE_IDS if profile in catalog["profiles"]
                ]
                keys = {
                    provider["key"]
                    for profile in profiles
                    for provider in catalog["profiles"][profile]
                    if provider["backend"] == backend.BACKEND_ID
                }
                parameters = {key: control_for(key, catalog)["default"] for key in keys}
                seen = set()
                for item in direct:
                    name, separator, value = item.partition("=")
                    control = control_for(name, catalog)
                    key = control["key"]
                    if (
                        not separator
                        or key not in keys
                        or key in seen
                        or not re.fullmatch(r"0|-?[1-9][0-9]*", value)
                    ):
                        raise ValueError("Invalid/duplicate DSP parameter: " + item)
                    parameters[key] = integer(control, int(value))
                    seen.add(key)
            if any(parameters.values()) and (not args.llvm_mc or not args.linker):
                parser.error("Set --llvm-mc and --linker to LLVM tools with Hexagon support")
            result = build_candidate(
                args.input, args.output, parameters, args.llvm_mc, args.linker, args.profile, plan
            )
        print(json.dumps(result, indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, f"TTP experiment refused: {error}\n")


if __name__ == "__main__":
    main()
