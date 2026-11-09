// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;
public final class CodecTuningTest {
    static int assertions;
    static final String HQ = TuningDefinitions.ADAPTIVE_HQ_TTP, LL = TuningDefinitions.ADAPTIVE_LL_TTP;
    static void check(boolean value) { assertions++; if (!value) throw new AssertionError(); }
    @SuppressWarnings("unchecked") static List<Map<String, Object>> operations(Map<String, Object> plan) {
        return (List<Map<String, Object>>)plan.get("operations");
    }
    static void rejects(Runnable run) {
        try { run.run(); throw new AssertionError("invalid request accepted"); }
        catch (IllegalArgumentException expected) { assertions++; }
    }
    public static void main(String[] args) {
        ExtensionConfig defaults = ExtensionConfig.defaults();
        for (String value : Arrays.asList("0", "1", "37", "50", "99", "100")) {
            ExtensionConfig config = defaults.with("lossless.hq.ttp_delta_ms", value).with(LL, "23");
            for (String codec : Arrays.asList("adaptive", "lossless")) {
                Map<String, Object> plan = CodecTuning.plan(config, "samsung-sm-s931z-dzif", codec, "hq", 44100, codec.equals("lossless") ? 16 : 24);
                Map<String, Object> operation = operations(plan).get(0);
                check(operation.get("value").equals(Integer.parseInt(value)));
                check(operation.get("key").equals(HQ));
                check(plan.get("applied").equals(false));
                check(operation.get("coupled_contexts").equals(Arrays.asList("adaptive/hq", "lossless/hq")));
            }
            Map<String, Object> ll = CodecTuning.plan(config, "samsung-sm-s931z-dzif", "adaptive", "ll", 48000, 24);
            check(operations(ll).get(0).get("value").equals(23));
            check(operations(ll).get(0).get("key").equals(LL));
            check(ExtensionConfig.parse(config.serialize(), true).value(HQ).equals(value));
        }
        for (String value : Arrays.asList("-1", "101", "050", "+50", "1.0", "system", "50ms", "2147483648"))
            rejects(() -> defaults.with(HQ, value));
        // All old spellings resolve to one key and never survive a save/output.
        for (String alias : Arrays.asList("aptx_hq_ttp_delta_ms", "adaptive.hq.ttp_delta_ms", "lossless.hq.ttp_delta_ms")) {
            check(CodecTuning.key(alias).equals(HQ));
            ExtensionConfig migrated = ExtensionConfig.parse("schema=2\n" + alias + "=37\nclassic_qhs_host_aware=true\n", false).forMigration();
            check(migrated.value(HQ).equals("37"));
            check(!migrated.qhsEnabled());
            check(migrated.serialize().contains(HQ + "=37"));
            check(!migrated.serialize().contains(alias + "="));
            check(defaults.with(alias, "9").value(alias).equals("9"));
            rejects(() -> ExtensionConfig.parse(HQ + "=37\n" + alias + "=37", true));
            rejects(() -> ExtensionConfig.parse(alias + "=37\n" + HQ + "=50", true));
        }
        check(ExtensionConfig.parse("aptx_hq_ttp_delta_ms=37\nclassic_qhs_host_aware=true", true).forMigration().qhsEnabled());
        for (String profile : Arrays.asList("samsung-sm-s931z-dzif", "another-phone"))
            for (String codec : Arrays.asList("adaptive", "aptx-ll", "aptx-hd", "lc3", "future-codec"))
                for (String mode : Arrays.asList("hq", "ll", "system")) {
                    Map<String, Object> plan = CodecTuning.plan(defaults, profile, codec, mode, 48000, 24);
                    boolean supported = profile.equals("samsung-sm-s931z-dzif") && codec.equals("adaptive") && !mode.equals("system");
                    check(operations(plan).size() == (supported ? 1 : 0));
                    check(plan.get("applied").equals(false));
                }
        check(operations(CodecTuning.plan(defaults, "samsung-sm-s931z-dzif", "lossless", "ll", 44100, 16)).isEmpty());
        check(operations(CodecTuning.plan(defaults, "samsung-sm-s931z-dzif", "lossless", "hq", 48000, 16)).isEmpty());
        check(operations(CodecTuning.plan(defaults, "samsung-sm-s931z-dzif", "adaptive", "hq", 32000, 20)).isEmpty());
        rejects(() -> CodecTuning.key("aptx-ll.ll.ttp_delta_ms"));
        independentCatalog();
        System.out.println("Codec tuning: " + assertions + " checks; canonical aliases/migration, integer domains, independent LL/HQ and multiple device/provider controls passed.");
    }
    static void independentCatalog() {
        // A different device/codec/rate/unit/domain and two controls in one context
        // exercise the actual planner without any Samsung/aptX branches.
        TuningCatalog.IntegerControl offset = new TuningCatalog.IntegerControl("test_offset_us", "us", 0, -20, 80, 5, "old_offset");
        TuningCatalog.IntegerControl frames = new TuningCatalog.IntegerControl("queue_frames", "frames", 1, 1, 255, 1);
        TuningCatalog.Context context = new TuningCatalog.Context("opus", "balanced", new Integer[] {32000}, new Integer[] {20});
        Map<String, List<TuningCatalog.Provider>> profiles = new LinkedHashMap<>();
        profiles.put("test-vendor-phone", Arrays.asList(
            new TuningCatalog.Provider("test-offset", "test-backend", offset.key, "fresh_graph", context),
            new TuningCatalog.Provider("test-queue", "test-backend", frames.key, "fresh_graph", context)));
        TuningCatalog catalog = new TuningCatalog(Arrays.asList(offset, frames), profiles);
        Map<String, Integer> values = new LinkedHashMap<>(); values.put(offset.key, -15); values.put(frames.key, 127);
        Map<String, Object> plan = CodecTuning.plan(catalog, values, "test-vendor-phone", "opus", "balanced", 32000, 20);
        check(operations(plan).size() == 2);
        check(operations(plan).get(0).get("value").equals(-15));
        check(operations(plan).get(1).get("unit").equals("frames"));
        check(plan.get("state").equals("planned_not_applied"));
        check(catalog.control("old_offset").key.equals(offset.key));
        check(operations(CodecTuning.plan(catalog, values, "samsung-sm-s931z-dzif", "opus", "balanced", 32000, 20)).isEmpty());
        rejects(() -> offset.parse("-14"));
        rejects(() -> frames.parse("0"));
        rejects(() -> frames.parse("256"));
        check(operations(CodecTuning.plan(catalog, Collections.emptyMap(), "test-vendor-phone", "opus", "balanced", 32000, 20)).get(1).get("value").equals(1));
        rejects(() -> new TuningCatalog(Arrays.asList(offset, frames, offset), profiles));
    }
}
