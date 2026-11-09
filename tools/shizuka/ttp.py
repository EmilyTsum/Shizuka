# SPDX-License-Identifier: GPL-2.0-only
"""Explicit DSP backend dispatch and versioned control-plan compatibility."""

import hashlib
from pathlib import Path

from .dsp_backends import samsung_dzif_ttp, samsung_dzif_verify
from .tuning import (
    context_names,
    control_for,
    integer,
    load_catalog,
    matches,
    validate_context,
    validate_plan,
)

# Backends are reviewed code, never modules/commands supplied by configuration.
BACKENDS = {samsung_dzif_ttp.BACKEND_ID: samsung_dzif_ttp}
VERIFIERS = {samsung_dzif_ttp.BACKEND_ID: samsung_dzif_verify.build_harness}


def resolve_backend(profile=None, stock_sha256=None, backend_id=None):
    candidates = [
        backend
        for name, backend in BACKENDS.items()
        if (backend_id is None or name == backend_id)
        and (profile is None or profile in backend.PROFILE_IDS)
        and (stock_sha256 is None or stock_sha256 == backend.STOCK_SHA256)
    ]
    if len(candidates) != 1:
        raise ValueError("Unsupported or ambiguous DSP profile/image/backend")
    return candidates[0]


def parameters_from_plan(plan):
    if isinstance(plan, dict) and type(plan.get("schema")) is int and plan["schema"] == 1:
        # Read-only adapter for previously exported HQ/LL plans. New output is schema 2.
        catalog = load_catalog()
        context, requested = plan.get("context"), plan.get("requested_ms")
        validate_context(context)
        providers = [
            provider
            for provider in catalog["profiles"].get(context.get("profile"), [])
            if provider["id"] == plan.get("provider") and matches(provider, context)
        ]
        if len(providers) != 1 or plan.get("applied") is not False:
            raise ValueError("Unsupported legacy tuning plan")
        provider = providers[0]
        integer(control_for(provider["key"], catalog), requested)
        if (
            plan.get("activation") != provider["activation"]
            or plan.get("coupled_contexts") != context_names(provider)
            or plan.get("state") != ("stock" if requested == 0 else "planned_not_applied")
        ):
            raise ValueError("Inconsistent legacy tuning plan")
        backend = resolve_backend(profile=context["profile"], backend_id=provider["backend"])
        # Legacy field names exist only at this adapter boundary.
        default = {"hq_ms": 0, "adaptive_ll_ms": 0}
        default["adaptive_ll_ms" if provider["key"] == backend.LL_KEY else "hq_ms"] = requested
        image = plan.get("image_request", default)
        if not isinstance(image, dict) or set(image) != {"hq_ms", "adaptive_ll_ms"}:
            raise ValueError("Invalid legacy image request")
        parameters = {backend.HQ_KEY: image["hq_ms"], backend.LL_KEY: image["adaptive_ll_ms"]}
        for key, value in parameters.items():
            integer(control_for(key, catalog), value)
        if parameters[provider["key"]] != requested:
            raise ValueError("Inconsistent legacy context/image request")
        return backend, parameters
    context, parameters, providers = validate_plan(plan)
    ids = {provider["backend"] for provider in providers}
    if len(ids) != 1:
        raise ValueError("A DSP image must have one reviewed backend")
    backend = resolve_backend(profile=context["profile"], backend_id=ids.pop())
    return backend, parameters


def plan_increments(plan):
    backend, parameters = parameters_from_plan(plan)
    return parameters[backend.HQ_KEY], parameters[backend.LL_KEY]


def validate_control_plan(plan):
    backend, parameters = parameters_from_plan(plan)
    if plan["schema"] == 1:
        return plan["requested_ms"]
    operations = plan["operations"]
    if len(operations) != 1:
        raise ValueError("Use parameters for a plan with multiple controls")
    return parameters[operations[0]["key"]]


def inspect_stock(data, profile=None):
    backend = resolve_backend(profile=profile, stock_sha256=hashlib.sha256(data).hexdigest())
    return backend.inspect_stock(data)


def write_candidate(
    stock, destination, delta_ms, llvm_mc, linker, control_plan=None, ll_delta_ms=0, profile=None
):
    backend = resolve_backend(
        profile=profile, stock_sha256=hashlib.sha256(Path(stock).read_bytes()).hexdigest()
    )
    if control_plan is not None:
        planned_backend, parameters = parameters_from_plan(control_plan)
        if backend is not planned_backend or (delta_ms, ll_delta_ms) != (
            parameters[backend.HQ_KEY],
            parameters[backend.LL_KEY],
        ):
            raise ValueError("DSP parameters differ from the control plan")
    return backend.write_candidate(
        stock, destination, delta_ms, llvm_mc, linker, control_plan, ll_delta_ms
    )


def build_candidate(
    stock, destination, parameters, llvm_mc, linker, profile=None, control_plan=None
):
    data = Path(stock).read_bytes()
    backend = resolve_backend(profile=profile, stock_sha256=hashlib.sha256(data).hexdigest())
    if control_plan is not None:
        selected, planned = parameters_from_plan(control_plan)
        if selected is not backend or parameters != planned:
            raise ValueError("Parameters/backend differ from the control plan")
    return backend.build_candidate(stock, destination, parameters, llvm_mc, linker, control_plan)


def verify_candidate(stock, candidate, manifest, output, llvm_mc, linker, profile=None):
    backend = resolve_backend(profile=profile, stock_sha256=hashlib.sha256(stock).hexdigest())
    verifier = VERIFIERS.get(backend.BACKEND_ID)
    if verifier is None:
        raise ValueError("No instruction verifier registered for this DSP backend")
    return verifier(candidate, manifest, stock, output, llvm_mc, linker)
