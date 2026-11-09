# SPDX-License-Identifier: GPL-2.0-only
"""Shared JDK discovery for Android builds and pure host checks."""

import os
import shutil
from pathlib import Path


def java_tools():
    home = os.environ.get("JAVA_HOME")
    java = str(Path(home) / "bin/java") if home else shutil.which("java")
    if not java:
        raise ValueError("A JDK 17 or newer is required (JAVA_HOME or java on PATH)")
    javac = str(Path(home) / "bin/javac") if home else shutil.which("javac")
    compiler = [javac] if javac and Path(javac).is_file() else [java, "com.sun.tools.javac.Main"]
    return java, compiler
