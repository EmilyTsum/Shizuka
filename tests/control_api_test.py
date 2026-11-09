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
from shizuka.ttp import plan_increments, validate_control_plan
from shizuka.tuning import load_catalog, validate_plan, verify_generated


def main():
    verify_generated()
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
        config.write_text("schema=2\naptx_hq_ttp_delta_ms=50\nadaptive_ll_ttp_delta_ms=100\n")
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
                ("parameters", {"adaptive_hq_ttp_delta_ms": 51, "adaptive_ll_ttp_delta_ms": 100}),
                ("operations", []),
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
        # Canonical and old configurations produce the same schema-2 plan.
        legacy = json.loads(
            subprocess.check_output([*command, "adaptive", "hq", "44100", "24"], text=True)
        )
        config.write_text("schema=2\nadaptive_hq_ttp_delta_ms=50\nadaptive_ll_ttp_delta_ms=100\n")
        canonical = json.loads(
            subprocess.check_output([*command, "adaptive", "hq", "44100", "24"], text=True)
        )
        assert canonical == legacy and canonical["schema"] == 2
        assert "aptx_hq" not in json.dumps(canonical)
        # Every integer in the declared domain crosses Java and Python identically.
        for hq, ll_value in ((0, 1), (1, 99), (37, 23), (100, 100)):
            config.write_text(
                f"adaptive_hq_ttp_delta_ms={hq}\nadaptive_ll_ttp_delta_ms={ll_value}\n"
            )
            integer_plan = json.loads(
                subprocess.check_output([*command, "adaptive", "ll", "48000", "24"], text=True)
            )
            assert plan_increments(integer_plan) == (hq, ll_value)
            validate_plan(integer_plan)
        config.write_text("adaptive_hq_ttp_delta_ms=50\nadaptive_ll_ttp_delta_ms=100\n")
        ll = json.loads(
            subprocess.check_output([*command, "adaptive", "ll", "48000", "24"], text=True)
        )
        assert validate_control_plan(ll) == 100
        assert plan_increments(ll) == (50, 100)
        # Previously exported plans remain input-only compatibility contracts.
        operation = ll["operations"][0]
        old_plan = dict(
            schema=1,
            context=ll["context"],
            provider=operation["provider"],
            requested_ms=100,
            applied=False,
            activation=operation["activation"],
            coupled_contexts=operation["coupled_contexts"],
            state="planned_not_applied",
        )
        assert plan_increments(old_plan) == (0, 100)
        old_plan["image_request"] = {"hq_ms": 50, "adaptive_ll_ms": 100}
        assert plan_increments(old_plan) == (50, 100)
        for mutate in (
            lambda p: p["parameters"].update(adaptive_ll_ttp_delta_ms=True),
            lambda p: p["operations"][0].update(unit="us"),
            lambda p: p["operations"][0].update(value=False),
            lambda p: p["context"].update(profile="another-phone"),
            lambda p: p["parameters"].update(adaptive_ll_ttp_delta_ms=101),
        ):
            bad = copy.deepcopy(ll)
            mutate(bad)
            try:
                plan_increments(bad)
            except ValueError:
                pass
            else:
                raise AssertionError("Tampered typed/context plan accepted")
        catalog = load_catalog()
        broken = copy.deepcopy(catalog)
        broken["controls"][1]["aliases"].append("adaptive_hq_ttp_delta_ms")
        catalog_file = root / "bad-catalog.json"
        catalog_file.write_text(json.dumps(broken))
        try:
            load_catalog(catalog_file)
        except ValueError:
            pass
        else:
            raise AssertionError("Colliding key/alias accepted")
        broken = copy.deepcopy(catalog)
        broken["schema"] = True
        catalog_file.write_text(json.dumps(broken))
        try:
            load_catalog(catalog_file)
        except ValueError:
            pass
        else:
            raise AssertionError("Boolean catalog schema accepted")
        unsupported = json.loads(
            subprocess.check_output([*command, "aptx-ll", "ll", "48000", "16"], text=True)
        )
        try:
            validate_control_plan(unsupported)
        except ValueError:
            pass
        else:
            raise AssertionError("Separate aptX LL codec adjustment plan accepted")
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
