# SPDX-License-Identifier: GPL-2.0-only
"""Round-trip the real portable Java plan/decoder through the external JSON boundary."""

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from codec_control import CORE, records
from shizuka.java import java_tools
from shizuka.project import ANDROID
from shizuka.ttp import validate_control_plan


def main():
    java, compiler = java_tools()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        subprocess.run(
            [
                *compiler,
                "--release",
                "8",
                "-d",
                directory,
                *[str(ANDROID / (name + ".java")) for name in CORE],
            ],
            check=True,
        )
        config = root / "config.conf"
        config.write_text("schema=2\naptx_hq_ttp_delta_ms=50\n")
        command = [
            java,
            "-cp",
            directory,
            "CodecControlTool",
            "plan",
            str(config),
            "samsung-sm-s931z-dzif",
        ]
        for codec, bits in (("adaptive", 24), ("lossless", 16)):
            plan = json.loads(
                subprocess.check_output([*command, codec, "hq", "44100", str(bits)], text=True)
            )
            assert validate_control_plan(plan) == 50
            for key, value in (
                ("applied", True),
                ("requested_ms", 51),
                ("coupled_contexts", ["lossless/hq"]),
                ("schema", True),
            ):
                bad = copy.deepcopy(plan)
                bad[key] = value
                try:
                    validate_control_plan(bad)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f"Invalid plan accepted: {key}")
        unsupported = json.loads(
            subprocess.check_output([*command, "adaptive", "ll", "48000", "24"], text=True)
        )
        try:
            validate_control_plan(unsupported)
        except ValueError:
            pass
        else:
            raise AssertionError("Unsupported LL plan accepted")
        raw = root / "raw.jsonl"
        raw.write_text(
            json.dumps(
                {
                    "type": "dsp_log",
                    "monotonic_s": 10.0,
                    "text": "SPF APTX_ADAPTIVE3_ENC:\nPacket- #1\nHQ State- STATE_ENCLEVEL_5_LOSSLESS\nInterval- 30.0\n",
                }
            )
            + "\n"
        )
        import base64

        protocol = "".join(
            f"{time}\t{base64.b64encode(text.encode()).decode()}\n"
            for time, text in records(raw, "native")
        )
        replay = subprocess.run(
            [java, "-cp", directory, "DspReplay"],
            input=protocol,
            text=True,
            capture_output=True,
            check=True,
        )
        final = json.loads(replay.stdout.splitlines()[-1])
        assert final["replay"] is True and final["available"] is False and final["live"] is False
        assert final["fields"]["encoder_lossless"]["value"] is True
        assert final["receiver_bit_perfect"] == "Unverified"
    print(
        "Control API: real Java/Python JSON plans, unsupported/tampered requests and explicitly offline Lossless replay passed"
    )


if __name__ == "__main__":
    main()
