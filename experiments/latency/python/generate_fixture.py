#!/usr/bin/env python3
"""Generate a deterministic synthetic RD pair; never reads a device/capture."""
import argparse
import json
from pathlib import Path

import encoded_ttp_relay as relay
from test_encoded_ttp_relay import metadata_for, packet


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    # One primary and one secondary wrapper in a single synthetic RD frame.
    data = packet(ttp=24625, flags=0xA0) + packet(ttp=0, flags=0xC0)
    metadata = metadata_for(data)
    expected = relay.relay_buffer(data, metadata, 150000,
                                  active_profile=relay.SUPPORTED_PROFILE)
    assert expected.report["wrapper_ttp_us_before"] == [197000, 0]
    assert expected.report["wrapper_ttp_us_after"] == [347000, 0]
    try:
        args.output_dir.mkdir(mode=0o700, exist_ok=False)
        (args.output_dir / "rd-data.bin").write_bytes(data)
        (args.output_dir / "rd-metadata.bin").write_bytes(metadata)
        (args.output_dir / "expected-report.json").write_text(
            json.dumps(expected.report, indent=2) + "\n")
    except OSError as exc:
        parser.exit(2, f"cannot create new synthetic fixture directory: {exc}\n")
    print(f"Synthetic fixture written to {args.output_dir}; no graph/device I/O")


if __name__ == "__main__":
    main()
