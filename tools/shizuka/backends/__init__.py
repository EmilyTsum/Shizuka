# SPDX-License-Identifier: GPL-2.0-only
"""Explicit backend registry, including each backend's supported profile identities."""

import importlib
from dataclasses import dataclass


@dataclass(frozen=True)
class BackendSpec:
    module: str
    profiles: frozenset


BACKENDS = {
    "samsung-dzif": BackendSpec("samsung_dzif", frozenset({"sm-s931z-dzif-aptx-experiment"})),
}


def get_backend(profile):
    spec = BACKENDS.get(profile.driver)
    if spec is None or profile.id not in spec.profiles:
        raise ValueError("Unsupported native backend/profile")
    backend = importlib.import_module("." + spec.module, __name__)
    for method in ("build", "host_cases", "host_flags", "verify_dispatch"):
        if not callable(getattr(backend, method, None)):
            raise ValueError(f"Backend {profile.driver} does not implement {method}")
    return backend
