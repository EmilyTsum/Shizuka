// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Device-independent typed controls and provider matching. No firmware addresses or codec rules. */
final class TuningCatalog {
    static final class IntegerControl {
        final String key, unit;
        final List<String> aliases;
        final int defaultValue, min, max, step;
        IntegerControl(String key, String unit, int defaultValue, int min, int max, int step, String... aliases) {
            this.key = key; this.unit = unit; this.defaultValue = defaultValue;
            this.min = min; this.max = max; this.step = step;
            this.aliases = Collections.unmodifiableList(Arrays.asList(aliases));
            if (min > max || step <= 0) throw new IllegalArgumentException("Invalid integer domain: " + key);
            parse(Integer.toString(defaultValue));
        }
        int parse(String text) {
            if (text == null || !text.matches("0|-?[1-9][0-9]*")) throw new IllegalArgumentException("invalid " + key);
            final int value;
            try { value = Integer.parseInt(text); }
            catch (NumberFormatException e) { throw new IllegalArgumentException("invalid " + key); }
            if (value < min || value > max || ((long)value - min) % step != 0)
                throw new IllegalArgumentException("invalid " + key + "; expected " + min + ".." + max + " step " + step + " " + unit);
            return value;
        }
        Map<String, Object> describe() {
            return ControlJson.object("key", key, "type", "integer", "unit", unit, "default", defaultValue,
                "min", min, "max", max, "step", step, "aliases", aliases);
        }
    }
    static final class Context {
        final String codec, mode;
        final List<Integer> rates, bits;
        Context(String codec, String mode, Integer[] rates, Integer[] bits) {
            this.codec = codec; this.mode = mode;
            this.rates = Collections.unmodifiableList(Arrays.asList(rates));
            this.bits = Collections.unmodifiableList(Arrays.asList(bits));
        }
        boolean matches(String codec, String mode, int rate, int bits) {
            return this.codec.equals(codec) && this.mode.equals(mode) && rates.contains(rate) && this.bits.contains(bits);
        }
        String name() { return codec + "/" + mode; }
        Map<String, Object> describe() {
            return ControlJson.object("codec", codec, "mode", mode, "sample_rates_hz", rates, "selection_bits", bits);
        }
    }
    static final class Provider {
        final String id, backend, key, activation;
        final List<Context> contexts;
        Provider(String id, String backend, String key, String activation, Context... contexts) {
            this.id = id; this.backend = backend; this.key = key; this.activation = activation;
            this.contexts = Collections.unmodifiableList(Arrays.asList(contexts));
        }
        List<String> contextNames() {
            List<String> names = new ArrayList<>();
            for (Context context : contexts) names.add(context.name());
            return names;
        }
        boolean matches(String codec, String mode, int rate, int bits) {
            for (Context context : contexts) if (context.matches(codec, mode, rate, bits)) return true;
            return false;
        }
        Map<String, Object> describe() {
            List<Object> details = new ArrayList<>();
            for (Context context : contexts) details.add(context.describe());
            return ControlJson.object("id", id, "backend", backend, "key", key, "activation", activation,
                "hot_apply", false, "contexts", details);
        }
    }
    final List<IntegerControl> controls;
    final Map<String, List<Provider>> profiles;
    private final Map<String, IntegerControl> byName = new LinkedHashMap<>();
    TuningCatalog(List<IntegerControl> controls, Map<String, List<Provider>> profiles) {
        this.controls = Collections.unmodifiableList(new ArrayList<>(controls));
        Map<String, List<Provider>> copy = new LinkedHashMap<>();
        for (IntegerControl control : controls) {
            List<String> names = new ArrayList<>(control.aliases); names.add(control.key);
            for (String name : names) if (byName.put(name, control) != null)
                throw new IllegalArgumentException("Duplicate tuning name: " + name);
        }
        for (Map.Entry<String, List<Provider>> entry : profiles.entrySet()) {
            Set<String> ids = new HashSet<>(), contexts = new HashSet<>();
            for (Provider provider : entry.getValue()) {
                if (!byName.containsKey(provider.key) || !provider.key.equals(control(provider.key).key) || !ids.add(provider.id))
                    throw new IllegalArgumentException("Invalid tuning provider: " + provider.id);
                for (Context context : provider.contexts) for (int rate : context.rates) for (int bits : context.bits)
                    if (!contexts.add(provider.key + "/" + context.name() + "/" + rate + "/" + bits))
                        throw new IllegalArgumentException("Ambiguous tuning context");
            }
            copy.put(entry.getKey(), Collections.unmodifiableList(new ArrayList<>(entry.getValue())));
        }
        this.profiles = Collections.unmodifiableMap(copy);
    }
    IntegerControl find(String key) { return byName.get(key); }
    IntegerControl control(String key) {
        IntegerControl control = find(key);
        if (control == null) throw new IllegalArgumentException("Unsupported tuning control: " + key + "; inspect tuning capabilities");
        return control;
    }
    List<Provider> providers(String profile) { return profiles.getOrDefault(profile, Collections.emptyList()); }
    List<Provider> resolve(String profile, String codec, String mode, int rate, int bits) {
        List<Provider> found = new ArrayList<>();
        for (Provider provider : providers(profile)) if (provider.matches(codec, mode, rate, bits)) found.add(provider);
        return found;
    }
}
