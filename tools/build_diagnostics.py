# SPDX-License-Identifier: GPL-2.0-only
"""Build the opt-in stock DSP log reader, independently of codec hook generation."""

import json
import subprocess

from shizuka.project import BUILD, ROOT, digest, ndk_bin


def build_diagnostics():
    output = BUILD / "dsp-capture"
    source = ROOT / "src/diagnostics/dsp_capture.c"
    subprocess.run(
        [
            str(ndk_bin() / "clang"),
            "--target=aarch64-linux-android34",
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fstack-protector-strong",
            str(source),
            "-o",
            str(output),
        ],
        check=True,
    )
    (BUILD / "diagnostics-build.json").write_text(
        json.dumps(
            {
                "source_sha256": digest(source),
                "builder_sha256": digest(__file__),
                "output_sha256": digest(output),
            },
            indent=2,
        )
        + "\n"
    )
    return output


if __name__ == "__main__":
    build_diagnostics()
