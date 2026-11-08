# SPDX-License-Identifier: GPL-2.0-only
import json
import os
import subprocess
from pathlib import Path

r = Path(__file__).resolve().parents[1]
base = [
    "clang++",
    "-std=c++17",
    "-g",
    "-fsanitize=address,undefined",
    "-DS25_R2=3",
    "-DS25_RATE=96000",
    "-DS25_MODE=4096",
]
jdk = Path(os.environ.get("JAVA_HOME", ""))
jni = [
    "-I" + str(jdk / "include"),
    "-I" + str(jdk / "include" / ("darwin" if __import__("sys").platform == "darwin" else "linux")),
]
if not (jdk / "include/jni.h").is_file():
    raise SystemExit("Set JAVA_HOME to a JDK with include/jni.h")
(r / "build").mkdir(parents=True, exist_ok=True)
(r / "evidence").mkdir(parents=True, exist_ok=True)
results = []
for name, files, flags in [
    ("ll_switch", [], []),
    ("aptx_ll", ["aptx_ll.cpp", "aptx_ll_backend.cpp"], []),
    ("ll_native_protocol", ["aptx_ll.cpp", "aptx_ll_native.cpp"], []),
    (
        "ll_ui_registration",
        ["ui.cpp", "protocol.cpp", "aptx_ll.cpp", "aptx_ll_native.cpp"],
        ["-DS25_UI_TEST_BASE", *jni],
    ),
    ("protocol", ["protocol.cpp"], []),
    ("peer_negotiation", ["protocol.cpp"], []),
    (
        "vendor",
        [
            "protocol.cpp",
            "payload.cpp",
            "layout.cpp",
            "aptx_ll.cpp",
            "aptx_ll_native.cpp",
        ],
        ["-DS25_MALLOC_GOT=0xea65e0"],
    ),
    (
        "ui_callback",
        ["ui.cpp", "protocol.cpp", "aptx_ll.cpp", "aptx_ll_native.cpp"],
        jni,
    ),
    ("layout_stability", ["layout.cpp", "protocol.cpp"], []),
]:
    extra = [str(r / "tests/ll_native_tramp_stubs.cpp")] if "aptx_ll_native.cpp" in files else []
    test = r / "tests" / f"{name}_test.cpp"
    out = r / "build" / f"host-{name}-test"
    cmd = (
        base + flags + extra + [str(test)] + [str(r / "src" / f) for f in files] + ["-o", str(out)]
    )
    subprocess.run(cmd, check=True)
    res = subprocess.run([str(out)], check=True, capture_output=True, text=True)
    print(name + ": " + res.stdout.strip())
    results.append({"test": name, "passed": True, "output": res.stdout.strip()})
# The controller request policy is pure Java; exercise async epochs without Android or HCI.
policy_classes = r / "build/policy-host-tests"
policy_classes.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [
        str(jdk / "bin/javac"),
        "-d",
        str(policy_classes),
        str(r / "tools/ClassicQhsPolicy.java"),
        str(r / "tests/ClassicQhsPolicyTest.java"),
    ],
    check=True,
)
policy_result = subprocess.run(
    [str(jdk / "bin/java"), "-cp", str(policy_classes), "ClassicQhsPolicyTest"],
    check=True,
    capture_output=True,
    text=True,
)
print(policy_result.stdout.strip())
results.append(
    {"test": "classic_qhs_policy", "passed": True, "output": policy_result.stdout.strip()}
)
(r / "evidence/ll-offline-tests.json").write_text(
    json.dumps(results, ensure_ascii=False, indent=2) + "\n"
)
env = dict(os.environ)
if "--host-only" not in __import__("sys").argv:
    subprocess.run([__import__("sys").executable, str(r / "tools/build.py")], env=env, check=True)
    subprocess.run(
        [__import__("sys").executable, str(r / "tests/dispatch_emulation_check.py")],
        env=env,
        check=True,
    )
