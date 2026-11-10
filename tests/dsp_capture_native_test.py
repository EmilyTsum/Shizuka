# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real decoder and detached timeout cleanup without Android/device access."""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    if sys.platform != "linux":
        raise SystemExit("DSP capture native tests require Linux pidfds and DMA headers")
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(
        os.environ.get("CC", shutil.which("clang") or shutil.which("gcc") or "cc")
    )
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "capture-test"
        subprocess.run(
            [
                *compiler,
                "-std=c11",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-function",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(root / "tests/dsp_capture_test.c"),
                "-o",
                str(output),
            ],
            check=True,
        )
        result = subprocess.check_output([str(output)], text=True, timeout=15)
        lines = result.splitlines()
        assert lines[-1] == "NATIVE_CAPTURE_PASSED"
        assert json.loads(lines[0])["text"] == 'A "quoted"\\line\nnext'
        print(
            "DSP capture: bounded/filtered records, detached timeout, owner death, automatic dead-owner recovery and live/unowned/replaced FARF preservation passed under ASan/UBSan"
        )


if __name__ == "__main__":
    main()
