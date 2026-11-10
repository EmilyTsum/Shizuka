#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the dormant application engine with synthetic owners and Android APIs."""

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APPLICATION = ROOT / "experiments/latency/application"


def main():
    for name in (
        "test_application.py",
        "test_android_session.py",
        "test_android_entry.py",
        "test_launcher.py",
        "test_fence.py",
        "test_owned_gate.py",
    ):
        subprocess.run([sys.executable, str(APPLICATION / name)], check=True)


if __name__ == "__main__":
    main()
