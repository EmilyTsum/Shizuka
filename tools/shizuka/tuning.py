# SPDX-License-Identifier: GPL-2.0-only
"""Portable control domains/provider contracts. Firmware code belongs in DSP backends."""

import json
import re
from pathlib import Path

from .project import ROOT

DEFINITIONS = ROOT / "config/tuning.json"
GENERATED = ROOT / "tools/android/TuningDefinitions.java"


def integer(control, value):
    if (
        type(value) is not int
        or not control["min"] <= value <= control["max"]
        or (value - control["min"]) % control["step"]
    ):
        raise ValueError(
            f"Invalid {control['key']}: expected {control['min']}..{control['max']} step {control['step']} {control['unit']}"
        )
    return value


def load_catalog(path=DEFINITIONS):
    catalog = json.loads(Path(path).read_text())
    if (
        not isinstance(catalog, dict)
        or set(catalog) != {"schema", "controls", "profiles"}
        or type(catalog["schema"]) is not int
        or catalog["schema"] != 1
    ):
        raise ValueError("Invalid tuning catalog schema")
    names, symbols, controls = set(), set(), {}
    for control in catalog["controls"]:
        if set(control) != {
            "symbol",
            "key",
            "aliases",
            "type",
            "unit",
            "default",
            "min",
            "max",
            "step",
        }:
            raise ValueError("Invalid tuning control fields")
        if (
            control["type"] != "integer"
            or not re.fullmatch(r"[A-Z][A-Z0-9_]*", control["symbol"])
            or control["symbol"] in symbols
        ):
            raise ValueError("Invalid tuning type/symbol")
        symbols.add(control["symbol"])
        if not isinstance(control["unit"], str) or not control["unit"]:
            raise ValueError("Missing tuning unit")
        for field in ("default", "min", "max", "step"):
            if type(control[field]) is not int or not -(2**31) <= control[field] < 2**31:
                raise ValueError("Integer control must fit signed 32 bits")
        if control["min"] > control["max"] or control["step"] <= 0:
            raise ValueError("Invalid tuning integer domain")
        integer(control, control["default"])
        for name in [control["key"], *control["aliases"]]:
            if (
                not isinstance(name, str)
                or not re.fullmatch(r"[a-z][a-z0-9_.]*", name)
                or name in names
            ):
                raise ValueError("Duplicate or invalid tuning key/alias")
            names.add(name)
        controls[control["key"]] = control
    for profile, providers in catalog["profiles"].items():
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", profile):
            raise ValueError("Invalid tuning profile")
        ids, contexts = set(), set()
        for provider in providers:
            if (
                set(provider) != {"id", "backend", "key", "activation", "contexts"}
                or provider["key"] not in controls
            ):
                raise ValueError("Invalid tuning provider")
            for field in ("id", "backend", "activation"):
                if not isinstance(provider[field], str) or not re.fullmatch(
                    r"[a-z][a-z0-9_-]*", provider[field]
                ):
                    raise ValueError("Invalid provider identifier")
            if provider["id"] in ids or not provider["contexts"]:
                raise ValueError("Duplicate/empty provider")
            ids.add(provider["id"])
            for context in provider["contexts"]:
                if set(context) != {"codec", "mode", "sample_rates_hz", "selection_bits"}:
                    raise ValueError("Invalid tuning context")
                for field in ("codec", "mode"):
                    if not isinstance(context[field], str) or not re.fullmatch(
                        r"[a-z0-9][a-z0-9-]*", context[field]
                    ):
                        raise ValueError("Invalid context identifier")
                for field in ("sample_rates_hz", "selection_bits"):
                    if (
                        not context[field]
                        or any(type(v) is not int or not 0 < v < 2**31 for v in context[field])
                        or len(set(context[field])) != len(context[field])
                    ):
                        raise ValueError("Invalid context integer list")
                for rate in context["sample_rates_hz"]:
                    for bits in context["selection_bits"]:
                        match = (provider["key"], context["codec"], context["mode"], rate, bits)
                        if match in contexts:
                            raise ValueError("Ambiguous tuning provider for a control/context")
                        contexts.add(match)
    return catalog


def control_for(name, catalog=None):
    for control in (catalog or load_catalog())["controls"]:
        if name == control["key"] or name in control["aliases"]:
            return control
    raise ValueError("Unknown tuning key: " + name)


def context_names(provider):
    return [context["codec"] + "/" + context["mode"] for context in provider["contexts"]]


def matches(provider, context):
    return any(
        context["codec"] == item["codec"]
        and context["mode"] == item["mode"]
        and context["sample_rate_hz"] in item["sample_rates_hz"]
        and context["selection_bits"] in item["selection_bits"]
        for item in provider["contexts"]
    )


def validate_context(context):
    if not isinstance(context, dict) or set(context) != {
        "profile",
        "codec",
        "mode",
        "sample_rate_hz",
        "selection_bits",
    }:
        raise ValueError("Invalid codec-control context")
    for field in ("profile", "codec", "mode"):
        if not isinstance(context[field], str) or not re.fullmatch(
            r"[a-z0-9][a-z0-9-]*", context[field]
        ):
            raise ValueError("Invalid context identifier")
    for field in ("sample_rate_hz", "selection_bits"):
        if type(context[field]) is not int or not 0 <= context[field] < 2**31:
            raise ValueError("Invalid context integer")


def validate_plan(plan, catalog=None):
    """Check a schema-2 plan without guessing device/codec support from a name."""
    catalog = catalog or load_catalog()
    if (
        not isinstance(plan, dict)
        or type(plan.get("schema")) is not int
        or plan["schema"] != 2
        or plan.get("applied") is not False
    ):
        raise ValueError("Invalid codec-control plan schema/application acknowledgement")
    context, parameters = plan.get("context"), plan.get("parameters")
    if not isinstance(context, dict) or set(context) != {
        "profile",
        "codec",
        "mode",
        "sample_rate_hz",
        "selection_bits",
    }:
        raise ValueError("Invalid codec-control context")
    validate_context(context)
    providers = catalog["profiles"].get(context["profile"], [])
    supported = [provider for provider in providers if matches(provider, context)]
    if not supported:
        raise ValueError("No tuning provider for this profile/context")
    keys = {provider["key"] for provider in providers}
    if not isinstance(parameters, dict) or set(parameters) != keys:
        raise ValueError("Invalid profile tuning parameters")
    for key, value in parameters.items():
        integer(control_for(key, catalog), value)
    operations = []
    for provider in supported:
        control = control_for(provider["key"], catalog)
        operations.append(
            dict(
                provider=provider["id"],
                backend=provider["backend"],
                key=control["key"],
                value=parameters[control["key"]],
                unit=control["unit"],
                activation=provider["activation"],
                coupled_contexts=context_names(provider),
            )
        )
    state = (
        "planned_not_applied"
        if any(op["value"] != control_for(op["key"], catalog)["default"] for op in operations)
        else "stock"
    )
    if not isinstance(plan.get("operations"), list) or any(
        not isinstance(op, dict) or type(op.get("value")) is not int for op in plan["operations"]
    ):
        raise ValueError("Invalid tuning operation types")
    if (
        plan.get("operations") != operations
        or plan.get("state") != state
        or plan.get("state_scope") != "context"
    ):
        raise ValueError("Inconsistent tuning operations/state")
    return context, parameters, supported


def generated_source(catalog=None):
    catalog = catalog or load_catalog()
    quote = json.dumps
    lines = [
        "// SPDX-License-Identifier: GPL-2.0-only",
        "// Generated by tools/generate_tuning.py from config/tuning.json. Do not edit.",
        "import java.util.*;",
        "final class TuningDefinitions {",
    ]
    for control in catalog["controls"]:
        lines.append(f"    static final String {control['symbol']} = {quote(control['key'])};")
    lines += [
        "    static final TuningCatalog CATALOG = create();",
        "    private static TuningCatalog create() {",
        "        List<TuningCatalog.IntegerControl> controls = Arrays.asList(",
    ]
    entries = []
    for control in catalog["controls"]:
        args = [
            quote(control["key"]),
            quote(control["unit"]),
            *[str(control[f]) for f in ("default", "min", "max", "step")],
            *map(quote, control["aliases"]),
        ]
        entries.append("            new TuningCatalog.IntegerControl(" + ", ".join(args) + ")")
    lines += [
        ",\n".join(entries) + ");",
        "        Map<String, List<TuningCatalog.Provider>> profiles = new LinkedHashMap<>();",
    ]
    for profile, providers in catalog["profiles"].items():
        entries = []
        for provider in providers:
            args = [quote(provider[f]) for f in ("id", "backend", "key", "activation")]
            for context in provider["contexts"]:
                arrays = [
                    "new Integer[] {" + ", ".join(map(str, context[f])) + "}"
                    for f in ("sample_rates_hz", "selection_bits")
                ]
                args.append(
                    "new TuningCatalog.Context("
                    + ", ".join([quote(context["codec"]), quote(context["mode"]), *arrays])
                    + ")"
                )
            entries.append("            new TuningCatalog.Provider(" + ", ".join(args) + ")")
        lines.append(
            "        profiles.put("
            + quote(profile)
            + ", Arrays.asList(\n"
            + ",\n".join(entries)
            + "));"
        )
    return "\n".join(
        [*lines, "        return new TuningCatalog(controls, profiles);", "    }", "}", ""]
    )


def verify_generated():
    if not GENERATED.is_file() or GENERATED.read_text() != generated_source():
        raise ValueError("Tuning definitions are stale; run tools/generate_tuning.py")
