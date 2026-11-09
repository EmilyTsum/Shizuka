// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Only extension controls live here. Android owns all per-device Bluetooth preferences. */
final class ExtensionConfig {
    static final String[] KEYS = {"schema", "classic_qhs_host_aware", "aptx_voice_enabled",
                                 "adaptive_priority", "aptx_hd_priority"};
    private static final Set<String> ALLOWED = new HashSet<>(Arrays.asList(KEYS));
    private static final Set<String> LEGACY_DEVICE_KEYS = new HashSet<>(Arrays.asList(
        "target_address", "startup_transport", "call_transport", "adaptive_apply_on_connect",
        "adaptive_rate", "adaptive_bits", "adaptive_mode", "media_allowed", "calls_allowed",
        "le_audio_allowed"));
    final boolean trusted;
    final boolean legacy;
    private final Map<String, String> values;

    private ExtensionConfig(Map<String, String> values, boolean trusted, boolean legacy) {
        this.values = Collections.unmodifiableMap(new LinkedHashMap<>(values));
        this.trusted = trusted;
        this.legacy = legacy;
    }
    static ExtensionConfig defaults() { return parse("", false); }
    static ExtensionConfig parse(String text, boolean trusted) {
        Map<String, String> input = new LinkedHashMap<>();
        for (String raw : text.split("\\r\\n|\\r|\\n")) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int separator = line.indexOf('=');
            if (separator < 1) throw new IllegalArgumentException("expected key=value");
            String key = line.substring(0, separator).trim();
            String value = line.substring(separator + 1).trim();
            if (input.put(key, value) != null)
                throw new IllegalArgumentException("duplicate key: " + key);
        }
        String schema = input.getOrDefault("schema", "2");
        boolean legacy = "1".equals(schema);
        if (!legacy && !"2".equals(schema)) throw new IllegalArgumentException("unsupported schema");
        for (String key : input.keySet())
            if (!ALLOWED.contains(key) && !(legacy && LEGACY_DEVICE_KEYS.contains(key)))
                throw new IllegalArgumentException("unknown extension key: " + key);
        Map<String, String> values = new LinkedHashMap<>();
        values.put("schema", "2");
        values.put("classic_qhs_host_aware", input.getOrDefault("classic_qhs_host_aware", "false"));
        for (String key : new String[] {"aptx_voice_enabled", "adaptive_priority", "aptx_hd_priority"}) {
            String value = input.getOrDefault(key, "system");
            // These were forced defaults, not system preferences. Do not replay them on upgrade.
            if (legacy && ((key.equals("aptx_voice_enabled") && value.equals("false")) ||
                           (!key.equals("aptx_voice_enabled") && value.equals("4001"))))
                value = "system";
            values.put(key, value);
        }
        validate(values);
        return new ExtensionConfig(values, trusted, legacy);
    }
    private static void validate(Map<String, String> values) {
        choice("classic_qhs_host_aware", values.get("classic_qhs_host_aware"), "true", "false");
        choice("aptx_voice_enabled", values.get("aptx_voice_enabled"), "system", "true", "false");
        for (String key : new String[] {"adaptive_priority", "aptx_hd_priority"})
            if (!values.get(key).equals("system") && !values.get(key).matches("[0-9]{1,6}"))
                throw new IllegalArgumentException("invalid " + key);
    }
    static void choice(String key, String value, String... allowed) {
        if (!Arrays.asList(allowed).contains(value))
            throw new IllegalArgumentException("invalid " + key);
    }
    String value(String key) {
        if (!ALLOWED.contains(key))
            throw new IllegalArgumentException("Android owns this preference; use device settings: " + key);
        return values.get(key);
    }
    boolean qhsEnabled() {
        return "true".equals(ClassicQhsPolicy.trustedOptInValue(value("classic_qhs_host_aware"), trusted));
    }
    ExtensionConfig forMigration() {
        Map<String, String> migrated = new LinkedHashMap<>(values);
        // An unrelated save must not turn an untrusted opt-in into a trusted one.
        migrated.put("classic_qhs_host_aware", Boolean.toString(qhsEnabled()));
        return new ExtensionConfig(migrated, trusted, false);
    }
    ExtensionConfig with(String key, String value) {
        if ("schema".equals(key) || !ALLOWED.contains(key))
            throw new IllegalArgumentException("only extension settings can be saved: " + key);
        Map<String, String> updated = new LinkedHashMap<>(forMigration().values);
        updated.put(key, value);
        validate(updated);
        return new ExtensionConfig(updated, trusted, false);
    }
    String serialize() {
        StringBuilder text = new StringBuilder("# Shizuka extensions; per-device preferences belong to Android.\n");
        for (String key : KEYS) text.append(key).append('=').append(values.get(key)).append('\n');
        return text.toString();
    }
}
