#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate portable Android definitions from the shared tuning catalog."""

import argparse

from shizuka.tuning import GENERATED, generated_source, verify_generated

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        verify_generated()
    else:
        GENERATED.write_text(generated_source())
