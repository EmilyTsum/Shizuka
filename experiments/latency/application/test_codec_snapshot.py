#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only exact codec preservation checks; no Android device or root."""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools

java, compiler = java_tools()
with tempfile.TemporaryDirectory(prefix="shizuka-codec-snapshot-") as output:
    subprocess.run([*compiler, "--release", "8", "-d", output,
                    str(HERE / "CodecSnapshot.java"), str(HERE / "CodecSnapshotTest.java")], check=True)
    subprocess.run([java, "-cp", output, "CodecSnapshotTest"], check=True)
