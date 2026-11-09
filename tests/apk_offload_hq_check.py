#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the APK HQ predicate against null/current-codec/preference matrices."""

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REL = "smali/com/android/bluetooth/a2dp/A2dpService.smali"
METHOD = ".method public checkHqCodecState(Landroid/bluetooth/BluetoothDevice;)Z"


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def method(text):
    return text.split(METHOD, 1)[1].split(".end method", 1)[0]


def execute(body, codec, prefs, status_present=True, config_present=True):
    """Small strict interpreter: execute actual smali branches, not a copied predicate."""
    lines = [x.strip() for x in body.splitlines() if x.strip() and not x.strip().startswith(".")]
    labels = {x: i for i, x in enumerate(lines) if x.startswith(":")}
    regs = {"p0": "service", "p1": "device"}
    result = None
    pc = 0
    preference_reads = 0
    while pc < len(lines):
        line = lines[pc]
        pc += 1
        if line.startswith(":"):
            continue
        if line.startswith("invoke-"):
            args = re.search(r"\{([^}]*)\}", line).group(1).split(", ")
            values = [regs[x] for x in args]
            name = line.split("->", 1)[1].split("(", 1)[0]
            if name == "getCodecStatus":
                result = "status" if status_present else None
            elif name == "getCodecConfig":
                result = "config" if config_present else None
            elif name == "getCodecType":
                result = codec
            elif name == "getCodecEnabled":
                preference_reads += 1
                result = prefs[values[-1]]
            elif name in ("<init>", "append", "toString", "d", "i"):
                result = "ignored-log"
            else:
                raise AssertionError(f"Unexpected method: {line}")
        elif line.startswith("move-result"):
            regs[line.split()[1]] = result
        elif line.startswith("const-string "):
            regs[line.split()[1].rstrip(",")] = "log-text"
        elif line.startswith("const/"):
            _, dst, value = line.split()
            regs[dst.rstrip(",")] = int(value, 0)
        elif line.startswith(("sget-object ", "new-instance ")):
            regs[line.split()[1].rstrip(",")] = "log-object"
        elif line.startswith("if-"):
            op, rest = line.split(" ", 1)
            operands = rest.split(", ")
            a = regs[operands[0]]
            b = 0 if op.endswith("z") else regs[operands[1]]
            # Dalvik null is the zero reference; integer zero also compares equal.
            equal = (a is None or a == 0) if op.endswith("z") else a == b
            take = equal if op.startswith("if-eq") else not equal
            if take:
                pc = labels[operands[-1]]
        elif line.startswith("goto"):
            pc = labels[line.split()[1]]
        elif line.startswith("return "):
            return regs[line.split()[1]], preference_reads
        else:
            raise AssertionError(f"Unsupported smali instruction: {line}")
    raise AssertionError("Method did not return")


def main():
    patch = next(
        x
        for x in json.loads((ROOT / "patches/bluetooth-apk.json").read_text())["files"]
        if x["path"] == REL
    )
    text = (ROOT / "build/bluetooth-apk" / REL).read_text()
    (edit,) = patch["edits"]
    prefix = edit["insert"]
    if digest(text) == patch["before_sha256"]:
        stock = text
        lines = text.splitlines(keepends=True)
        lines[edit["line"] : edit["line"]] = prefix.splitlines(keepends=True)
        patched = "".join(lines)
    else:
        assert digest(text) == patch["after_sha256"]
        patched = text
        assert text.count(prefix) == 1
        stock = text.replace(prefix, "", 1)
    assert digest(stock) == patch["before_sha256"]
    assert digest(patched) == patch["after_sha256"]
    assert patched.replace(prefix, "", 1) == stock  # All other guards/methods identical.
    before, after = method(stock), method(patched)
    cases = 0
    for codec in range(-1, 17):
        for ldac in (-1, 0, 1):
            for ssc in (-1, 0, 1):
                for status, config in ((False, False), (True, False), (True, True)):
                    prefs = {4: ldac, 8: ssc}
                    expected = execute(before, codec, prefs)[0]
                    bypass = status and config and codec in (7, 12)
                    actual, reads = execute(after, codec, prefs, status, config)
                    assert actual == (0 if bypass else expected), (codec, prefs, status, config)
                    if bypass:
                        assert reads == 0
                    cases += 1
    print(
        f"APK HQ offload predicate: {cases} executed cases passed; null/other codecs retain stock behavior, public7/12 bypass only unrelated HQ preference veto"
    )


if __name__ == "__main__":
    main()
