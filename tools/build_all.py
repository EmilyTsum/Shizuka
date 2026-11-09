# SPDX-License-Identifier: GPL-2.0-only
"""Rebuild Android artifacts, run native checks, and create a local test module."""

import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def main():
    for script in ("build_android.py", "verify_offline.py", "package.py"):
        subprocess.run([sys.executable, str(root / "tools" / script)], check=True)


if __name__ == "__main__":
    main()
