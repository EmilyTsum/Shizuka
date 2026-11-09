#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the DZIF Classic prerequisite against Android connection policies."""

import hashlib
import itertools
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REL = "smali/com/android/bluetooth/btservice/AdapterService.smali"
HELPER = "s25RequiresClassicAudioProfile"
DESCRIPTOR = "(Landroid/bluetooth/BluetoothDevice;I)Z"
METHOD = "isAllSupportedClassicAudioProfilesActive(Landroid/bluetooth/BluetoothDevice;)Z"


def method(text, name):
    match = re.search(r"^\.method[^\n]* " + re.escape(name) + r"\n", text, re.M)
    assert match, name
    return text[match.end() : text.index(".end method", match.end())]


def execute(body, fixture, helper, arguments=None):
    lines = [
        line.strip()
        for line in body.splitlines()
        if line.strip() and not line.strip().startswith(".")
    ]
    labels = {line: i for i, line in enumerate(lines) if line.startswith(":")}
    registers = {"p0": "adapter", "p1": "device", **(arguments or {})}
    result = None
    pc = 0
    for _ in range(500):
        line = lines[pc]
        pc += 1
        if line.startswith((":", "check-cast")):
            continue
        if line.startswith("invoke-"):
            args = re.search(r"\{([^}]*)\}", line).group(1).split(", ")
            values = [registers[arg] for arg in args]
            name = line.split("->")[1].split("(")[0]
            if name == HELPER:
                result = execute(helper, fixture, helper, {"p2": values[2]})
            elif name == "isProfileSupported":
                result = int(fixture[values[2]]["supported"])
            elif name == "getProfileConnectionPolicy":
                result = fixture[values[2]]["policy"]
            elif name in ("getLeAudioService", "getHeadsetService", "getA2dpService"):
                kind = {"getLeAudioService": 22, "getHeadsetService": 1, "getA2dpService": 2}[name]
                result = ("optional", kind if fixture[kind]["present"] else None)
            elif name in ("isEmpty", "isPresent"):
                result = int((values[0][1] is None) == (name == "isEmpty"))
            elif name == "get":
                result = values[0][1]
                assert result is not None
            elif name == "getGroupDevices":
                result = ["device"]
            elif name == "getActiveDevice":
                result = fixture[values[0]]["active"]
            elif name == "contains":
                result = int(values[1] in values[0])
            else:
                raise AssertionError("Unexpected call: " + line)
        elif line.startswith("move-result"):
            registers[line.split()[1]] = result
        elif line.startswith("const/"):
            _, dst, value = line.split()
            registers[dst.rstrip(",")] = int(value, 0)
        elif line.startswith(("if-eqz ", "if-nez ")):
            operation, operands = line.split(" ", 1)
            register, label = operands.split(", ")
            zero = registers[register] in (None, 0)
            if zero == (operation == "if-eqz"):
                pc = labels[label]
        elif line.startswith("return "):
            return registers[line.split()[1]]
        else:
            raise AssertionError("Unsupported smali instruction: " + line)
    raise AssertionError("Smali execution did not terminate")


def main():
    definition = json.loads(
        (ROOT / "profiles/samsung-sm-s931z-dzif/bluetooth-apk.json").read_text()
    )
    entry = next(item for item in definition["files"] if item["path"] == REL)
    insertion = next(
        edit["insert"] for edit in entry["edits"] if ".method private " + HELPER in edit["insert"]
    )
    helper = method(insertion, HELPER + DESCRIPTOR)
    replacements = [
        edit
        for edit in entry["edits"]
        if "invoke-direct" in edit["insert"]
        and "->" + HELPER in edit["insert"]
        and ".method" not in edit["insert"]
    ]
    assert len(replacements) == 2 and all(edit["delete"] == 1 for edit in replacements)
    helper_cases = 0
    for profile, supported, policy in itertools.product((1, 2), (False, True), (-1, 0, 100)):
        fixture = {profile: {"supported": supported, "policy": policy}}
        assert execute(helper, fixture, helper, {"p2": profile}) == int(supported and policy != 0)
        helper_cases += 1
    decoded = ROOT / "build/bluetooth-apk" / REL
    if not decoded.is_file():
        print(
            f"LE route policy: {helper_cases} executed helper cases passed; full APK matrix runs during Android build"
        )
        return
    text = decoded.read_text()
    assert hashlib.sha256(text.encode()).hexdigest() == entry["after_sha256"]
    assert method(text, HELPER + DESCRIPTOR) == helper
    patched = method(text, METHOD)
    assert patched.count("->" + HELPER + DESCRIPTOR) == 2
    stock = patched.replace("invoke-direct", "invoke-virtual").replace(
        "->" + HELPER + DESCRIPTOR, "->isProfileSupported" + DESCRIPTOR
    )
    cases = 0
    fixed = 0
    dimensions = (
        (False, True),
        (False, True),
        (False, True),
        (-1, 0, 100),
        (-1, 0, 100),
        (None, "device", "other"),
        (None, "device", "other"),
        (False, True),
        (False, True),
    )
    for le, hfp, a2dp, hp, ap, ha, aa, hs, ads in itertools.product(*dimensions):
        fixture = {
            22: {"present": le},
            1: {"supported": hfp, "policy": hp, "active": ha, "present": hs},
            2: {"supported": a2dp, "policy": ap, "active": aa, "present": ads},
        }
        before = execute(stock, fixture, helper)
        after = execute(patched, fixture, helper)
        expected = (
            le
            and (not (hfp and hs and hp != 0) or ha == "device")
            and (not (a2dp and ads and ap != 0) or aa == "device")
        )
        assert after == int(expected), fixture
        if hp != 0 and ap != 0:
            assert before == after, fixture
        fixed += before == 0 and after == 1
        cases += 1
    assert fixed > 0
    print(
        f"LE route policy: {helper_cases} helper and {cases} actual APK gate cases passed; forbidden Classic profiles no longer block LE, allowed/unknown profiles retain the stock active-device checks"
    )


if __name__ == "__main__":
    main()
