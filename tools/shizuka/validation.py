# SPDX-License-Identifier: GPL-2.0-only
"""Shared validation contract: backend cases plus Android and lifecycle checks."""

from .backends import get_backend
from .project import offline_sources_fingerprint

POLICY_CASES = (
    ("dsp_decoder", "DspLogDecoderTest"),
    ("codec_tuning", "CodecTuningTest"),
    ("diagnostic_store", "DiagnosticStoreTest"),
    ("classic_qhs_policy", "ClassicQhsPolicyTest"),
    ("controller_features", "ControllerFeaturesTest"),
    ("system_preferences", "SystemPreferencesTest"),
    ("codec_presentation", "CodecPresentationTest"),
    ("playback_selection", "PlaybackSelectionTest"),
    ("settings_bridge", "BridgeProtocolTest"),
    ("extension_store", "ExtensionConfigStoreTest"),
    ("profile_session", "ProfileSessionTest"),
)
PYTHON_CASES = (
    ("dsp_capture_native", "dsp_capture_native_test.py"),
    ("dsp_observation_ui", "dsp_observation_ui_test.py"),
    ("settings_bootstrap", "settings_bootstrap_test.py"),
    ("native_app_install", "native_app_install_test.py"),
    ("control_api", "control_api_test.py"),
    ("ttp_trial_guards", "ttp_trial_test.py"),
    ("apk_handoff", "apk_handoff_check.py"),
    ("apk_le_route", "apk_le_route_check.py"),
    ("qhs_nvm", "qhs_nvm_test.py"),
    ("module_lifecycle", "module_lifecycle_test.py"),
    ("latency_runtime_lifecycle", "latency_runtime_lifecycle_test.py"),
    ("latency_request", "latency_request_test.py"),
    ("latency_request_owned", "latency_request_owned_test.py"),
    ("latency_application", "latency_application_test.py"),
    ("latency_remote_recovery", "latency_remote_recovery_test.py"),
    ("latency_profile_recovery", "latency_profile_recovery_test.py"),
    ("latency_apply_ui", "latency_apply_ui_test.py"),
    ("latency_control_facade", "latency_control_facade_test.py"),
    ("latency_runtime_native", "latency_runtime_native_test.py"),
    ("latency_artifacts", "latency_artifacts_test.py"),
    ("latency_health_recovery", "latency_health_recovery_test.py"),
    ("device_profiles", "device_profiles_test.py"),
    ("build_pipeline", "build_pipeline_test.py"),
    ("policy_lifecycle", "policy_lifecycle_test.py"),
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
