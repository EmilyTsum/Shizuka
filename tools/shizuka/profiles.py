# SPDX-License-Identifier: GPL-2.0-only
"""Explicit device profiles. A profile never makes another device binary-compatible."""

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from .backends import BACKENDS
from .project import ROOT, digest, inputs_dir, option

DEFAULT_PROFILE = "samsung-sm-s931z-dzif"
# A new native backend must be reviewed and registered, not imported from arbitrary JSON.


@dataclass(frozen=True)
class DeviceProfile:
    path: Path
    data: dict

    @property
    def id(self):
        return self.data["id"]

    @property
    def driver(self):
        return self.data["driver"]

    @property
    def runtime_id(self):
        return self.data["runtime_profile"]

    @property
    def fingerprint(self):
        encoded = json.dumps(self.data, sort_keys=True, separators=(",", ":")).encode()
        return hashlib.sha256(encoded).hexdigest()

    @property
    def targets(self):
        return self.data["targets"]

    @property
    def apk_patches(self):
        return self.path.with_name("bluetooth-apk.json")

    def verify_inputs(self, directory=None):
        directory = Path(directory) if directory is not None else inputs_dir()
        for target in self.targets:
            path = directory / target["local_file"]
            if not path.is_file() or digest(path) != target["sha256"]:
                raise ValueError(
                    f"Stock input missing or does not match profile {self.id}: {path.name}"
                )

    def input_manifest(self):
        return "".join(f"{target['sha256']} {target['path']}\n" for target in self.targets)


def load_profile(selector=None):
    selected = str(selector or option("PROFILE", DEFAULT_PROFILE))
    if re.fullmatch(r"[a-z0-9][a-z0-9-]*", selected):
        path = ROOT / "profiles" / selected / "profile.json"
    else:
        path = Path(selected).expanduser().resolve()
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or not all(
        isinstance(data.get(key), str) for key in ("id", "driver", "runtime_profile")
    ):
        raise ValueError("Profile must define string id, driver and runtime_profile fields")
    backend = BACKENDS.get(data.get("driver"))
    if data.get("schema") != 1 or backend is None or data.get("id") not in backend.profiles:
        raise ValueError(
            "Unsupported native profile/backend; register and verify a device backend first"
        )
    runtime = data.get("runtime_profile", "")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", runtime):
        raise ValueError("Invalid runtime profile identifier")
    if not (ROOT / "module/profiles" / (runtime + ".sh")).is_file():
        raise ValueError(f"Runtime backend is missing: {runtime}")
    targets = data.get("targets")
    if not isinstance(targets, list) or not targets:
        raise ValueError("Profile requires stock targets")
    names, paths = set(), set()
    for target in targets:
        if not isinstance(target, dict) or not all(
            isinstance(target.get(key), str) for key in ("local_file", "path", "sha256", "role")
        ):
            raise ValueError("Stock targets require string filename, path, digest and role fields")
        name, destination, sha = (
            target.get("local_file", ""),
            target.get("path", ""),
            target.get("sha256", ""),
        )
        if (
            not name
            or Path(name).name != name
            or name in {".", ".."}
            or any(c.isspace() for c in name)
        ):
            raise ValueError("Stock local_file must be one filename")
        if (
            not destination.startswith("/")
            or ".." in PurePosixPath(destination).parts
            or any(c.isspace() for c in destination)
        ):
            raise ValueError("Invalid absolute stock target")
        if not re.fullmatch(r"[0-9a-f]{64}", sha) or target.get("role") not in {
            "patch",
            "dependency",
        }:
            raise ValueError("Invalid stock digest/role")
        if name in names or destination in paths:
            raise ValueError("Duplicate stock target")
        names.add(name)
        paths.add(destination)
    payloads = data.get("payloads", {})
    if not isinstance(payloads, dict) or not all(
        isinstance(value, str) for value in payloads.values()
    ):
        raise ValueError("Payloads must map roles to filenames")
    if set(payloads) != {"native", "hal", "apk", "properties"} or len(set(payloads.values())) != 4:
        raise ValueError("Profile requires four distinct payload outputs")
    for name in payloads.values():
        if (
            not name
            or Path(name).name != name
            or name in {".", ".."}
            or any(c.isspace() for c in name)
        ):
            raise ValueError("Invalid payload output filename")
    return DeviceProfile(path, data)
