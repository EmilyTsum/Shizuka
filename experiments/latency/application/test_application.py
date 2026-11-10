#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile/run the real portable application policy; synthetic adapters only."""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools

java, compiler = java_tools()
with tempfile.TemporaryDirectory(prefix="shizuka-application-test-") as output:
    sources = [*sorted(HERE.glob("*.java")), HERE / "android/InstalledRootSource.java", *(ROOT / "tools/android" / name for name in ("ControlJson.java", "ExtensionConfig.java", "CodecTuning.java", "TuningCatalog.java", "TuningDefinitions.java", "ClassicQhsPolicy.java"))]
    subprocess.run([*compiler, "--release", "8", "-d", output, *map(str, sources)], check=True)
    for test in ("CodecSnapshotTest", "CodecTargetEligibilityTest", "ApplyOperationTest", "ApplyStoreTest", "ApplyStatusTest", "ApplyCommandTest", "GuardianChannelTest", "RootFactsTest", "ApplicationOwnerWriterTest", "InstalledRootSourceTest", "ApplicationFenceTest", "ApplicationReconcilerTest", "ApplyEntryTest", "ReadOnlyApplyAdapterTest"):
        subprocess.run([java, "-cp", output, test], check=True)
