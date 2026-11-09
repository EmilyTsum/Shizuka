# SPDX-License-Identifier: GPL-2.0-only
"""Shared validation contract: backend cases plus Android and lifecycle checks."""

from .backends import get_backend
from .project import offline_sources_fingerprint

POLICY_CASES = (
    ("classic_qhs_policy", "ClassicQhsPolicyTest"),
    ("controller_features", "ControllerFeaturesTest"),
    ("system_preferences", "SystemPreferencesTest"),
)
PYTHON_CASES = (
    ("qhs_nvm", "qhs_nvm_test.py"),
    ("module_lifecycle", "module_lifecycle_test.py"),
    ("device_profiles", "device_profiles_test.py"),
)


def required_tests(profile):
    common = {name for name, _ in (*POLICY_CASES, *PYTHON_CASES)}
    native = [name for name, _, _ in get_backend(profile).host_cases([])]
    if not native or len(native) != len(set(native)) or common.intersection(native):
        raise ValueError("Backend must declare distinct native validation cases")
    return common.union(native)


def validate_evidence(tests, profile):
    expected = required_tests(profile)
    if not isinstance(tests, list) or not all(isinstance(test, dict) for test in tests):
        raise ValueError("Invalid offline evidence; run tools/verify_offline.py")
    names = [test.get("test") for test in tests]
    if (
        not all(isinstance(name, str) for name in names)
        or len(names) != len(expected)
        or set(names) != expected
        or not all(test.get("passed") is True for test in tests)
    ):
        raise ValueError("Incomplete or failed offline evidence; run tools/verify_offline.py")
    source = offline_sources_fingerprint()
    identity = (profile.id, profile.fingerprint, source)
    if any(
        (test.get("profile_id"), test.get("profile_fingerprint"), test.get("sources_fingerprint"))
        != identity
        for test in tests
    ):
        raise ValueError(
            "Offline evidence is stale or belongs to another profile; run tools/verify_offline.py"
        )
