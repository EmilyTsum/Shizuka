#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native UI foreground ownership without Android or root."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools

java, compiler = java_tools()
with tempfile.TemporaryDirectory(prefix="shizuka-observation-ui-") as directory:
    subprocess.run([
        *compiler, "--release", "8", "-d", directory,
        str(ROOT / "tools/android-app/src/dev/shizuka/settings/DspObservationSession.java"),
        str(ROOT / "tools/android-app/src/dev/shizuka/settings/ObservationFreshness.java"),
        str(ROOT / "tests/DspObservationSessionTest.java"),
    ], check=True)
    subprocess.run([java, "-cp", directory, "dev.shizuka.settings.DspObservationSessionTest"], check=True)
