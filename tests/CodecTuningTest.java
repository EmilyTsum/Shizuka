// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;
public final class CodecTuningTest {
    static int assertions;
    static void check(boolean value) { assertions++; if (!value) throw new AssertionError(); }
    public static void main(String[] args) {
        ExtensionConfig defaults = ExtensionConfig.defaults();
        for (String value : Arrays.asList("0", "50", "100")) {
            ExtensionConfig config = defaults.with(CodecTuning.key("lossless.hq.ttp_delta_ms"), value);
            for (String codec : Arrays.asList("adaptive", "lossless")) {
                Map<String, Object> plan = CodecTuning.plan(config, CodecPresentation.SAMSUNG_DZIF, codec, "hq", 44100, codec.equals("lossless") ? 16 : 24);
                check(plan.get("requested_ms").equals(Integer.parseInt(value)));
                check(plan.get("applied").equals(false));
                check(plan.get("coupled_contexts").equals(CodecTuning.COUPLED));
            }
            check(ExtensionConfig.parse(config.serialize(), true).value(CodecTuning.KEY).equals(value));
        }
        for (String value : Arrays.asList("-1", "51", "200", "050", "system", "50ms")) {
            try { defaults.with(CodecTuning.KEY, value); throw new AssertionError(); } catch (IllegalArgumentException expected) { assertions++; }
        }
        check(CodecTuning.key("adaptive.hq.ttp_delta_ms").equals(CodecTuning.key("lossless.hq.ttp_delta_ms")));
        for (String value : Arrays.asList("0", "50", "100")) {
            ExtensionConfig independent = defaults.with(CodecTuning.KEY, "100").with(CodecTuning.key("adaptive.ll.ttp_delta_ms"), value);
            Map<String, Object> ll = CodecTuning.plan(independent, CodecPresentation.SAMSUNG_DZIF, "adaptive", "ll", 48000, 24);
            check(ll.get("requested_ms").equals(Integer.parseInt(value)));
            check(ll.get("provider").equals(CodecTuning.LL_PROVIDER));
            check(ll.get("coupled_contexts").equals(CodecTuning.LL_CONTEXTS));
            check(independent.value(CodecTuning.KEY).equals("100"));
            check(ll.get("applied").equals(false));
        }
        for (String profile : Arrays.asList(CodecPresentation.SAMSUNG_DZIF, "unsupported"))
            for (String codec : Arrays.asList("adaptive", "aptx-ll", "aptx-hd", "lc3"))
                for (String mode : Arrays.asList("hq", "ll", "system")) {
                    Map<String, Object> plan = CodecTuning.plan(defaults.with(CodecTuning.KEY, "100"), profile, codec, mode, 48000, 24);
                    boolean supported = profile.equals(CodecPresentation.SAMSUNG_DZIF) && codec.equals("adaptive") && mode.equals("hq");
                    check(plan.get("requested_ms").equals(supported ? 100 : 0));
                    check(plan.get("applied").equals(false));
                }
        try { CodecTuning.plan(defaults, CodecPresentation.SAMSUNG_DZIF, "lossless", "ll", 44100, 16); throw new AssertionError(); }
        catch (IllegalArgumentException expected) { assertions++; }
        try { CodecTuning.key("aptx-ll.ll.ttp_delta_ms"); throw new AssertionError(); }
        catch (IllegalArgumentException expected) { assertions++; }
        System.out.println("Codec tuning: " + assertions + " checks; independent Adaptive LL/shared HQ controls, typed values and intent/application separation passed.");
    }
}
