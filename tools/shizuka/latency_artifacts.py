# SPDX-License-Identifier: GPL-2.0-only
"""Separate identities for inactive, optional HAL-owned latency artifacts."""

import hashlib
import io
import json
from pathlib import Path

from elftools.common.exceptions import ELFError
from elftools.elf.elffile import ELFFile

from .artifacts import verify_native_build
from .project import BUILD, ROOT, digest

LATENCY_BUILD = "latency-runtime-build.json"
REVIEWED_DEPENDENCIES = {
    "/vendor/lib64/libar-gsl.so": "4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc",
    "/vendor/lib64/libar-gpr.so": "1339b2bcda1415535ec53666ceb4f873c3a7d33faf2a3cb55af3702a0a306d15",
    "/vendor/lib64/liblx-osal.so": "1d81e09e7eb9b9442218127036acb255f56857cadd220ed85dbed532619586ef",
}
EXTRA_OUTPUTS = (
    "latency-provider",
    "patched-libar-gsl.so",
    "latency-runtime-payload.elf",
    "latency-runtime-payload.build.json",
    "latency-runtime-patch.json",
    "latency-provider.build.json",
)
PROVIDER_COMPONENT_SOURCES = {
    "experiments/latency/provider/src/record.cpp",
    "experiments/latency/provider/src/status.cpp",
    "experiments/latency/provider/src/atomic_publisher.cpp",
    "experiments/latency/provider/src/main.cpp",
    "experiments/latency/provider/include/shizuka/provider.hpp",
    "experiments/latency/native/src/external_config.cpp",
}


def _fingerprint(root, paths):
    value = hashlib.sha256()
    for path in sorted(set(paths)):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Latency source is missing or linked: {path}")
        value.update(path.relative_to(root).as_posix().encode() + b"\0")
        value.update(path.read_bytes() + b"\0")
    return value.hexdigest()


def latency_sources_fingerprints(root=ROOT):
    root = Path(root)
    base = root / "experiments/latency"
    runtime = [
        *(p for p in (base / "runtime").rglob("*") if p.suffix in {".c", ".h", ".S", ".ld", ".py"}),
        *(p for p in (base / "transport/native").rglob("*") if p.suffix in {".c", ".h"}),
        *(p for p in (base / "gsl_hook").rglob("*") if p.suffix in {".c", ".h", ".py"}),
    ]
    provider = [
        *(p for p in (base / "provider").rglob("*") if p.suffix in {".cpp", ".hpp", ".py"}),
        base / "native/src/external_config.cpp",
        base / "native/include/shizuka/external_config.hpp",
    ]
    tooling = [root / "tools/build_latency_runtime.py", root / "tools/shizuka/latency_artifacts.py"]
    if not runtime or not provider:
        raise ValueError("Latency source tree is incomplete")
    return {
        "runtime": _fingerprint(root, runtime),
        "provider": _fingerprint(root, provider),
        "tooling": _fingerprint(root, tooling),
    }


def hal_context_rva(path):
    elf = ELFFile(io.BytesIO(Path(path).read_bytes()))
    if elf["e_machine"] != "EM_AARCH64" or not elf.little_endian or elf.elfclass != 64:
        raise ValueError("Matching HAL payload must be a little-endian ARM64 ELF")
    table = elf.get_section_by_name(".symtab")
    symbols = (
        []
        if table is None
        else [s for s in table.iter_symbols() if s.name == "s25_latency_context"]
    )
    if len(symbols) != 1 or symbols[0]["st_shndx"] == "SHN_UNDEF":
        raise ValueError("Matching HAL payload has no unique defined latency context")
    symbol = symbols[0]
    rva, size = symbol["st_value"], symbol["st_size"]
    if (
        not rva
        or rva % 4
        or size != 64
        or not any(
            segment["p_type"] == "PT_LOAD"
            and segment["p_flags"] & 2
            and segment["p_vaddr"] <= rva
            and rva + size <= segment["p_vaddr"] + segment["p_memsz"]
            for segment in elf.iter_segments()
        )
    ):
        raise ValueError("HAL latency context is outside a writable payload mapping")
    return rva


def verify_hal_payload(payload, patched):
    rva = hal_context_rva(payload)
    expected = ELFFile(io.BytesIO(Path(payload).read_bytes()))
    actual = ELFFile(io.BytesIO(Path(patched).read_bytes()))
    if actual["e_machine"] != "EM_AARCH64" or not actual.little_endian:
        raise ValueError("Patched HAL is not the matching ARM64 artifact")
    count = 0
    for segment in expected.iter_segments():
        if segment["p_type"] != "PT_LOAD" or not segment["p_memsz"]:
            continue
        count += 1
        matches = [
            s
            for s in actual.iter_segments()
            if s["p_type"] == "PT_LOAD"
            and all(s[key] == segment[key] for key in ("p_vaddr", "p_filesz", "p_memsz", "p_flags"))
        ]
        if len(matches) != 1 or matches[0].data() != segment.data():
            raise ValueError("HAL payload ELF does not match the bytes embedded in the patched HAL")
    if not count:
        raise ValueError("HAL payload has no loadable mappings")
    return rva


def core_latency_identity(profile, build=BUILD):
    if profile.runtime_id != "samsung-sm-s931z-dzif":
        raise ValueError("The optional latency runtime supports only the reviewed DZIF profile")
    verify_native_build(profile, build)
    names = {role: name for role, name in profile.data["payloads"].items() if role != "apk"}
    if "hal" not in names or "native" not in names:
        raise ValueError("The selected core build has no native/HAL artifact pair")
    for name in ["native-build.json", *names.values()]:
        if (build / name).is_symlink() or not (build / name).is_file():
            raise ValueError(f"Core latency input is missing or linked: {name}")
    hal = build / "hal-payload.elf"
    if hal.is_symlink() or not hal.is_file():
        raise ValueError("Matching build/hal-payload.elf is missing")
    return {
        "native_build_sha256": digest(build / "native-build.json"),
        "outputs": {role: digest(build / name) for role, name in names.items()},
        "hal_payload_sha256": digest(hal),
        "hal_context_rva": verify_hal_payload(hal, build / names["hal"]),
    }


def latency_cookie(
    profile, sources, core, *, diagnostic_zero_ttp=False, experimental_raw_island=False
):
    identity = {
        "format": "shizuka-latency-cookie-v1",
        "profile_id": profile.id,
        "profile_fingerprint": profile.fingerprint,
        "sources": sources,
        "core": core,
        "dependencies": REVIEWED_DEPENDENCIES,
        "diagnostic_only": diagnostic_zero_ttp or experimental_raw_island,
        "diagnostic_zero_ttp": diagnostic_zero_ttp,
        "experimental_raw_island": experimental_raw_island,
    }
    raw = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()
    cookie = hashlib.sha256(raw).hexdigest()[:32]
    if cookie == "0" * 32:
        cookie = hashlib.sha256(raw + b"\0nonzero").hexdigest()[:32]
    if cookie == "0" * 32:
        raise ValueError("Cannot derive a nonzero latency artifact identity")
    return cookie


def latency_identity(
    profile, build=BUILD, root=ROOT, *, diagnostic_zero_ttp=False, experimental_raw_island=False
):
    if type(diagnostic_zero_ttp) is not bool or type(experimental_raw_island) is not bool:
        raise ValueError("Diagnostic build mode must be a boolean")
    sources = latency_sources_fingerprints(root)
    core = core_latency_identity(profile, build)
    return {
        "schema": 1,
        "kind": "shizuka-latency-runtime",
        "profile_id": profile.id,
        "profile_fingerprint": profile.fingerprint,
        "sources": sources,
        "core": core,
        "cookie": latency_cookie(
            profile,
            sources,
            core,
            diagnostic_zero_ttp=diagnostic_zero_ttp,
            experimental_raw_island=experimental_raw_island,
        ),
        "dependencies": REVIEWED_DEPENDENCIES.copy(),
        "runtime_mode": "relay",
        "activation_requested": False,
        "device_proof": False,
        "diagnostic_only": diagnostic_zero_ttp or experimental_raw_island,
        "diagnostic_zero_ttp": diagnostic_zero_ttp,
        "experimental_raw_island": experimental_raw_island,
    }


def reject_diagnostic_manifest(metadata):
    """Production packaging never accepts diagnostic or ambiguous build modes."""
    if not isinstance(metadata, dict) or any(
        metadata.get(key) is not False
        for key in ("diagnostic_only", "diagnostic_zero_ttp", "experimental_raw_island")
    ):
        raise ValueError("Diagnostic or unmarked latency artifacts cannot be packaged")


def _runtime_mode(path, name):
    """Read the compiled mode, independently of mutable JSON build records."""
    elf = ELFFile(io.BytesIO(Path(path).read_bytes()))
    table = elf.get_section_by_name(".symtab")
    symbols = [] if table is None else [s for s in table.iter_symbols() if s.name == name]
    if (
        elf["e_machine"] != "EM_AARCH64"
        or not elf.little_endian
        or elf.elfclass != 64
        or len(symbols) != 1
        or symbols[0]["st_shndx"] != "SHN_ABS"
        or symbols[0]["st_value"] not in (0, 1)
    ):
        raise ValueError("Runtime payload has no exact compiled diagnostic mode")
    return bool(symbols[0]["st_value"])


def runtime_diagnostic_zero_ttp(path):
    return _runtime_mode(path, "shizuka_runtime_diagnostic_zero_ttp_mode")


def runtime_experimental_raw_island(path):
    return _runtime_mode(path, "shizuka_runtime_raw_island_mode")


def verify_runtime_embedding(payload_path, candidate_path):
    """A normal payload record must describe the bytes actually embedded in GSL."""
    payload = ELFFile(io.BytesIO(Path(payload_path).read_bytes()))
    candidate = ELFFile(io.BytesIO(Path(candidate_path).read_bytes()))
    if (
        candidate["e_machine"] != "EM_AARCH64"
        or not candidate.little_endian
        or candidate.elfclass != 64
    ):
        raise ValueError("Patched GSL is not the matching ARM64 artifact")
    count = 0
    for segment in payload.iter_segments():
        if segment["p_type"] != "PT_LOAD" or not segment["p_memsz"]:
            continue
        count += 1
        matches = [
            s
            for s in candidate.iter_segments()
            if s["p_type"] == "PT_LOAD"
            and all(s[key] == segment[key] for key in ("p_vaddr", "p_filesz", "p_memsz", "p_flags"))
        ]
        if len(matches) != 1 or matches[0].data() != segment.data():
            raise ValueError("Runtime payload does not match the bytes embedded in patched GSL")
    if not count:
        raise ValueError("Runtime payload has no loadable mappings")


def verify_latency_runtime(profile, build=BUILD, root=ROOT, *, allow_diagnostic=False):
    advice = "run tools/build_latency_runtime.py for the matching native/HAL build"
    try:
        path = build / LATENCY_BUILD
        if path.is_symlink() or not path.is_file():
            raise ValueError("Missing latency build record")
        metadata = json.loads(path.read_text())
        diagnostic = metadata.get("diagnostic_zero_ttp") if isinstance(metadata, dict) else None
        raw_island = metadata.get("experimental_raw_island") if isinstance(metadata, dict) else None
        diagnostic_only = bool(diagnostic or raw_island)
        if not allow_diagnostic:
            reject_diagnostic_manifest(metadata)
        elif (
            type(diagnostic) is not bool
            or type(raw_island) is not bool
            or metadata.get("diagnostic_only") is not diagnostic_only
        ):
            raise ValueError("Invalid diagnostic latency build identity")
        expected = latency_identity(
            profile, build, root, diagnostic_zero_ttp=diagnostic, experimental_raw_island=raw_island
        )
        if (
            not isinstance(metadata, dict)
            or set(metadata) != set(expected) | {"outputs"}
            or any(metadata.get(k) != v for k, v in expected.items())
        ):
            raise ValueError("Stale or mixed-source latency runtime identity")
        outputs = {}
        for name in EXTRA_OUTPUTS:
            path = build / name
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"Missing or linked latency artifact: {name}")
            outputs[name] = digest(path)
        if metadata.get("outputs") != outputs:
            raise ValueError("Latency artifacts changed after their build")
        if runtime_diagnostic_zero_ttp(build / "latency-runtime-payload.elf") is not diagnostic:
            raise ValueError(
                "Runtime payload compiled diagnostic mode does not match its build identity"
            )
        if runtime_experimental_raw_island(build / "latency-runtime-payload.elf") is not raw_island:
            raise ValueError(
                "Runtime payload compiled RAW-island mode does not match its build identity"
            )
        verify_runtime_embedding(
            build / "latency-runtime-payload.elf", build / "patched-libar-gsl.so"
        )
        payload = json.loads((build / "latency-runtime-payload.build.json").read_text())
        patch = json.loads((build / "latency-runtime-patch.json").read_text())
        provider = json.loads((build / "latency-provider.build.json").read_text())
        if not all(isinstance(value, dict) for value in (payload, patch, provider)):
            raise ValueError("Latency component build records must be JSON objects")
        if (
            payload.get("cookie") != expected["cookie"]
            or payload.get("relay_enabled") is not True
            or payload.get("observer_only") is not False
            or payload.get("attach_enabled") is not True
            or payload.get("hal_context_rva") != expected["core"]["hal_context_rva"]
            or payload.get("sha256") != outputs["latency-runtime-payload.elf"]
            or payload.get("gpr_initialized_by_runtime") is not False
            or payload.get("device_opened_by_runtime") is not False
            or payload.get("diagnostic_only") is not diagnostic_only
            or payload.get("diagnostic_zero_ttp") is not diagnostic
            or payload.get("experimental_raw_island") is not raw_island
        ):
            raise ValueError("Runtime payload build flags, cookie or HAL context do not match")
        if (
            patch.get("stock_sha256") != REVIEWED_DEPENDENCIES["/vendor/lib64/libar-gsl.so"]
            or patch.get("candidate_sha256") != outputs["patched-libar-gsl.so"]
            or patch.get("payload_sha256") != outputs["latency-runtime-payload.elf"]
            or patch.get("relay_enabled") is not True
            or patch.get("observer_only") is not False
            or patch.get("loaded_on_device") is not False
            or patch.get("new_gpr_transport") is not False
        ):
            raise ValueError("Patched GSL does not match its reviewed runtime inputs")
        if (
            provider.get("schema") != 1
            or provider.get("android") is not True
            or provider.get("sha256") != outputs["latency-provider"]
            or provider.get("gpr_linked") is not False
            or provider.get("device_executed") is not False
            or not isinstance(provider.get("sources_sha256"), dict)
            or set(provider["sources_sha256"]) != PROVIDER_COMPONENT_SOURCES
        ):
            raise ValueError("Latency provider does not match its Android build")
        for relative, expected_hash in provider["sources_sha256"].items():
            source = Path(relative)
            if (
                source.is_absolute()
                or ".." in source.parts
                or digest(root / source) != expected_hash
            ):
                raise ValueError("Provider component source identity changed")
        return metadata
    except (OSError, ValueError, TypeError, KeyError, ELFError) as error:
        raise ValueError(f"{error}; {advice}") from error
