#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the actual DZIF handoff helper against unavailable/rejected targets."""

import hashlib
import itertools
import json
import re
from pathlib import Path

from apk_le_route_check import method

ROOT = Path(__file__).resolve().parents[1]
REL = "smali/com/android/bluetooth/le_audio/LeAudioService.smali"
HELPER = "s25PrepareClassicHandoff(Landroid/bluetooth/BluetoothDevice;)V"


def execute(body, fixture):
    lines = [
        line.strip()
        for line in body.splitlines()
        if line.strip() and not line.strip().startswith(".")
    ]
    labels = {line: index for index, line in enumerate(lines) if line.startswith(":")}
    registers = {"p0": "le", "p1": "target" if fixture["target"] else None}
    pc, result, operations = 0, None, []
    for _ in range(300):
        line = lines[pc]
        pc += 1
        if line.startswith((":", "check-cast")):
            continue
        if line == "return-void":
            return operations
        if line.startswith("iget-object "):
            if "->mActiveAudioOutDevice:" in line:
                value = "lead" if fixture["active"] else None
            else:
                assert "->mAudioManager:" in line
                value = "audio"
            registers[line.split()[1].rstrip(",")] = value
        elif line.startswith("invoke-"):
            arguments = re.search(r"\{([^}]*)\}", line).group(1).split(", ")
            values = [registers[arg] for arg in arguments]
            name = line.split("->")[1].split("(")[0]
            if name == "getMode":
                result = fixture["audio_mode"]
            elif name == "getGroupId":
                result = fixture["source_group" if values[1] == "lead" else "target_group"]
            elif name == "getAdapterService":
                result = "adapter"
            elif name == "getA2dpService":
                result = "optional"
            elif name == "isEmpty":
                result = int(not fixture["present"])
            elif name == "get":
                assert fixture["present"]
                result = "a2dp"
            elif name == "getConnectionPolicy":
                result = fixture["policy"]
            elif name == "getConnectionState":
                result = fixture["connection"]
            elif name == "setActiveDevice":
                operations.append(("activate_classic", values[1]))
                result = fixture["accepted"]
            elif name == "getActiveDevice":
                result = fixture["readback"]
            elif name == "equals":
                result = int(values[0] == values[1])
            elif name == "removeActiveDevice":
                operations.append(("remove_le", values[1]))
                result = True
            else:
                raise AssertionError("Unexpected call: " + line)
        elif line.startswith("move-result"):
            registers[line.split()[1]] = result
        elif line.startswith("const/"):
            _, destination, value = line.split()
            registers[destination.rstrip(",")] = int(value, 0)
        elif line.startswith("if-"):
            operation, tail = line.split(" ", 1)
            operands = tail.split(", ")
            first = registers[operands[0]]
            equal = (
                first in (None, 0) if operation.endswith("z") else first == registers[operands[1]]
            )
            if equal == operation.startswith("if-eq"):
                pc = labels[operands[-1]]
        else:
            raise AssertionError("Unsupported instruction: " + line)
    raise AssertionError("Handoff did not terminate")


def main():
    profile = json.loads((ROOT / "profiles/samsung-sm-s931z-dzif/bluetooth-apk.json").read_text())
    entry = next(item for item in profile["files"] if item["path"] == REL)
    insertion = next(
        edit["insert"] for edit in entry["edits"] if ".method private " + HELPER in edit["insert"]
    )
    body = method(insertion, HELPER)
    dimensions = (
        (False, True),
        (False, True),
        (-1, 1, 2),
        (-1, 1, 2),
        (False, True),
        (-1, 0, 100),
        (0, 1, 2, 3),
        (False, True),
        (None, "target", "other"),
        (0, 1, 2, 3),
    )
    cases = 0
    for values in itertools.product(*dimensions):
        fixture = dict(
            zip(
                (
                    "target",
                    "active",
                    "source_group",
                    "target_group",
                    "present",
                    "policy",
                    "connection",
                    "accepted",
                    "readback",
                    "audio_mode",
                ),
                values,
            )
        )
        operations = execute(body, fixture)
        eligible = (
            fixture["target"]
            and fixture["audio_mode"] == 0
            and fixture["active"]
            and fixture["source_group"] == fixture["target_group"]
            and fixture["source_group"] != -1
            and fixture["present"]
            and fixture["policy"] == 100
            and fixture["connection"] == 2
        )
        expected = [("activate_classic", "target")] if eligible else []
        if eligible and fixture["accepted"] and fixture["readback"] == "target":
            expected.append(("remove_le", 1))
        assert operations == expected, (fixture, operations, expected)
        cases += 1
    decoded = ROOT / "build/bluetooth-apk" / REL
    if decoded.is_file():
        text = decoded.read_text()
        assert hashlib.sha256(text.encode()).hexdigest() == entry["after_sha256"]
        assert method(text, HELPER) == body
        setter = method(text, "setConnectionPolicy(Landroid/bluetooth/BluetoothDevice;I)Z")
        forbidden = setter.split("\n    :cond_3\n", 1)[1].split("\n    :cond_4\n", 1)[0]
        assert "if-nez p2, :cond_4" in forbidden
        assert (
            forbidden.index("->s25PrepareClassicHandoff(")
            < forbidden.index("->setEnabledState(")
            < forbidden.index("->disconnect(")
        )
    print(
        f"LE/Classic handoff: {cases} actual helper cases passed; only an allowed, connected, confirmed target suppresses LE removal noise; no profile permissions or player commands are written"
    )


if __name__ == "__main__":
    main()
