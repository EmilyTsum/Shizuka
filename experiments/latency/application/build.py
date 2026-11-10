#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build isolated preparation DEX to an external directory; does not package/deploy."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", required=True, type=Path)
args = parser.parse_args()
output = args.output_dir.resolve()
if output == ROOT or ROOT in output.parents:
    parser.error("Build output must be outside the checkout")
sdk = Path(os.environ.get("ANDROID_SDK_ROOT", ""))
versions = sorted((sdk / "build-tools").glob("*/d8"))
if not versions:
    parser.error("ANDROID_SDK_ROOT must contain build-tools/d8")
java, compiler = java_tools()
sources = sorted(p for p in HERE.glob("*.java") if not p.name.endswith("Test.java"))
sources.extend(sorted((HERE / "android").glob("*.java")))
sources.extend(ROOT / "tools/android" / name for name in ("ControlJson.java", "BluetoothRuntime.java", "ExtensionConfig.java", "CodecTuning.java", "TuningCatalog.java", "TuningDefinitions.java", "ClassicQhsPolicy.java"))
android_jar = sdk / "platforms/android-35/android.jar"
if not android_jar.is_file():
    parser.error("ANDROID_SDK_ROOT must contain platforms/android-35/android.jar")
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="shizuka-apply-build-") as temporary:
    classes = Path(temporary) / "classes"
    classes.mkdir()
    subprocess.run([*compiler, "--release", "8", "-classpath", str(android_jar), "-d", str(classes), *map(str, sources)], check=True)
    dex = Path(temporary) / "dex"
    dex.mkdir()
    subprocess.run([str(versions[-1]), "--min-api", "34", "--output", str(dex),
                    *map(str, sorted(classes.rglob("*.class")))], check=True)
    data = (dex / "classes.dex").read_bytes()
    (output / "latency-application.dex").write_bytes(data)
digest = hashlib.sha256(data).hexdigest()
(output / "latency-application.sha256").write_text(digest + "\n")
fingerprint = hashlib.sha256()
for path in sorted(sources):
    fingerprint.update(str(path.relative_to(ROOT)).encode() + b"\0" + path.read_bytes() + b"\0")
(output / "application-build.json").write_text(json.dumps({
    "schema": 1, "qualified_executor": False, "sha256": digest,
    "source_fingerprint": fingerprint.hexdigest(),
    "sources": [str(p.relative_to(ROOT)) for p in sources],
}, indent=2) + "\n")
print(f"Built unqualified preparation DEX: {digest}")
