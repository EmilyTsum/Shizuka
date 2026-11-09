// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Shared internal/CLI planning API. Providers declare support; saved intent is never applied. */
final class CodecTuning {
    static final TuningCatalog CATALOG = TuningDefinitions.CATALOG;
    static String key(String name) { return CATALOG.control(name).key; }
    static String canonicalOrOriginal(String name) {
        TuningCatalog.IntegerControl control = CATALOG.find(name);
        return control == null ? name : control.key;
    }
    static boolean isControl(String name) { return CATALOG.find(name) != null; }
    static String value(String name, String input) {
        return Integer.toString(CATALOG.control(name).parse(input));
    }
    static Map<String, Object> capabilities() { return capabilities(CATALOG); }
    static Map<String, Object> capabilities(TuningCatalog catalog) {
        List<Object> controls = new ArrayList<>();
        for (TuningCatalog.IntegerControl control : catalog.controls) controls.add(control.describe());
        Map<String, Object> profiles = new LinkedHashMap<>();
        for (String profile : catalog.profiles.keySet()) {
            List<Object> providers = new ArrayList<>();
            for (TuningCatalog.Provider provider : catalog.providers(profile)) providers.add(provider.describe());
            profiles.put(profile, providers);
        }
        return ControlJson.object("schema", 2, "controls", controls, "profiles", profiles,
            "scope", "Additional tuning support; not a playback-support matrix",
            "system_owned", Arrays.asList("codec selection", "sample rate", "bit depth", "preferred transport"),
            "observation", "diagnostics.sh capture; status.sh; diagnostics.sh export", "applied", false);
    }
    static Map<String, Object> describe(ExtensionConfig config) {
        Map<String, Object> requested = new LinkedHashMap<>();
        for (TuningCatalog.IntegerControl control : CATALOG.controls)
            requested.put(control.key, control.parse(config.value(control.key)));
        return ControlJson.object("schema", 2, "requested", requested, "applied", false,
            "activation", "provider_dependent; inspect capabilities and create a context plan");
    }
    static Map<String, Object> plan(ExtensionConfig config, String profile, String codec, String mode, int rate, int bits) {
        Map<String, Integer> values = new LinkedHashMap<>();
        for (TuningCatalog.IntegerControl control : CATALOG.controls)
            values.put(control.key, control.parse(config.value(control.key)));
        return plan(CATALOG, values, profile, codec, mode, rate, bits);
    }
    // Internal callers can supply a separately reviewed catalog without changing this planner.
    static Map<String, Object> plan(TuningCatalog catalog, Map<String, Integer> values, String profile, String codec, String mode, int rate, int bits) {
        for (String name : Arrays.asList(profile, codec, mode))
            if (name == null || !name.matches("[a-z0-9][a-z0-9-]*")) throw new IllegalArgumentException("Invalid context identifier");
        if (rate < 0 || bits < 0) throw new IllegalArgumentException("Invalid rate/bit-depth context");
        Map<String, Object> parameters = new LinkedHashMap<>();
        for (TuningCatalog.Provider provider : catalog.providers(profile)) {
            TuningCatalog.IntegerControl control = catalog.control(provider.key);
            Integer requested = values.get(provider.key);
            if (requested == null) requested = control.defaultValue;
            parameters.put(control.key, control.parse(Integer.toString(requested)));
        }
        List<TuningCatalog.Provider> supported = catalog.resolve(profile, codec, mode, rate, bits);
        List<Object> operations = new ArrayList<>();
        boolean changed = false;
        for (TuningCatalog.Provider provider : supported) {
            TuningCatalog.IntegerControl control = catalog.control(provider.key);
            int value = (Integer)parameters.get(control.key);
            changed |= value != control.defaultValue;
            operations.add(ControlJson.object("provider", provider.id, "backend", provider.backend,
                "key", control.key, "value", value, "unit", control.unit, "activation", provider.activation,
                "coupled_contexts", provider.contextNames()));
        }
        return ControlJson.object("schema", 2,
            "context", ControlJson.object("profile", profile, "codec", codec, "mode", mode, "sample_rate_hz", rate, "selection_bits", bits),
            "operations", operations, "parameters", parameters,
            "state", supported.isEmpty() ? "unsupported" : changed ? "planned_not_applied" : "stock",
            "state_scope", "context", "applied", false);
    }
}
