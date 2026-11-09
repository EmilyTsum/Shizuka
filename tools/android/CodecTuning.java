// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Shared internal/CLI planning API. Saved intent is never an applied acknowledgement. */
final class CodecTuning {
    static final String KEY = "aptx_hq_ttp_delta_ms";
    static final String LL_KEY = "adaptive_ll_ttp_delta_ms";
    static final String PROVIDER = "samsung-dzif-calculated-hq-ttp";
    static final String LL_PROVIDER = "samsung-dzif-calculated-adaptive-ll-ttp";
    static final List<String> COUPLED = Collections.unmodifiableList(
        Arrays.asList("adaptive/hq", "lossless/hq"));
    static final List<String> LL_CONTEXTS = Collections.singletonList("adaptive/ll");
    static String key(String name) {
        if (Arrays.asList(KEY, "adaptive.hq.ttp_delta_ms", "lossless.hq.ttp_delta_ms").contains(name)) return KEY;
        if (Arrays.asList(LL_KEY, "adaptive.ll.ttp_delta_ms").contains(name)) return LL_KEY;
        throw new IllegalArgumentException("Unsupported tuning control: " + name + "; inspect tuning capabilities");
    }
    static String value(String input) {
        return value(KEY, input);
    }
    static String value(String key, String input) {
        ExtensionConfig.choice(key, input, "0", "50", "100");
        return input;
    }
    static Map<String, Object> capabilities() {
        return ControlJson.object("schema", 1, "controls", Arrays.asList(
            ControlJson.object("key", KEY, "unit", "ms", "values", Arrays.asList(0, 50, 100),
                "default", 0, "provider", PROVIDER, "coupled_contexts", COUPLED,
                "aliases", Arrays.asList("adaptive.hq.ttp_delta_ms", "lossless.hq.ttp_delta_ms"),
                "activation", "offline_dsp_build_then_fresh_graph", "hot_apply", false,
                "profile", CodecPresentation.SAMSUNG_DZIF),
            ControlJson.object("key", LL_KEY, "unit", "ms", "values", Arrays.asList(0, 50, 100),
                "default", 0, "provider", LL_PROVIDER, "coupled_contexts", LL_CONTEXTS,
                "aliases", Collections.singletonList("adaptive.ll.ttp_delta_ms"),
                "activation", "offline_dsp_build_then_fresh_graph", "hot_apply", false,
                "profile", CodecPresentation.SAMSUNG_DZIF)),
            "scope", "Additional TTP adjustment support; not a playback-support matrix",
            "unsupported", Arrays.asList("separate aptX LL codec TTP adjustment", "independent Adaptive/Lossless HQ increments", "receiver buffer depth control"),
            "system_owned", Arrays.asList("codec selection", "sample rate", "bit depth", "preferred transport"),
            "observation", "diagnostics.sh capture; status.sh; diagnostics.sh export",
            "applied", false);
    }
    static Map<String, Object> describe(ExtensionConfig config) {
        return ControlJson.object("schema", 1, "requested", ControlJson.object(KEY, Integer.parseInt(config.value(KEY)), LL_KEY, Integer.parseInt(config.value(LL_KEY))),
            "applied", false, "activation", "offline_dsp_build_then_fresh_graph", "coupled_contexts", COUPLED);
    }
    static Map<String, Object> plan(ExtensionConfig config, String profile, String codec, String mode, int rate, int bits) {
        ExtensionConfig.choice("codec", codec, "adaptive", "lossless", "aptx-ll", "aptx-hd", "lc3");
        ExtensionConfig.choice("mode", mode, "hq", "ll", "system");
        if (!Arrays.asList(0, 44100, 48000, 96000).contains(rate) || !Arrays.asList(0, 16, 24, 32).contains(bits))
            throw new IllegalArgumentException("Unsupported rate/bit-depth context");
        if ("lossless".equals(codec) && (!"hq".equals(mode) || rate != 44100 || bits != 16))
            throw new IllegalArgumentException("Lossless selection requires HQ / 44100 / 16; DSP input remains 24 bit");
        boolean hq = "hq".equals(mode) && ("adaptive".equals(codec) || "lossless".equals(codec));
        boolean ll = "ll".equals(mode) && "adaptive".equals(codec);
        boolean complete = rate != 0 && ("lossless".equals(codec) ? bits == 16 : bits == 24);
        boolean supported = (hq || ll) && complete && CodecPresentation.SAMSUNG_DZIF.equals(profile);
        int hqIncrement = Integer.parseInt(config.value(KEY)), llIncrement = Integer.parseInt(config.value(LL_KEY));
        int increment = ll ? llIncrement : hqIncrement;
        return ControlJson.object("schema", 1,
            "context", ControlJson.object("profile", profile, "codec", codec, "mode", mode, "sample_rate_hz", rate, "selection_bits", bits),
            "provider", supported ? (ll ? LL_PROVIDER : PROVIDER) : null,
            "requested_ms", supported ? increment : 0,
            "state", !supported ? "unsupported" : increment == 0 ? "stock" : "planned_not_applied",
            "state_scope", "context",
            "reason", supported ? "Build a verified candidate and measure a fresh DSP graph" : "No TTP provider for this profile/codec/mode",
            "coupled_contexts", supported ? (ll ? LL_CONTEXTS : COUPLED) : Collections.emptyList(),
            "image_request", ControlJson.object("hq_ms", hqIncrement, "adaptive_ll_ms", llIncrement),
            "activation", "offline_dsp_build_then_fresh_graph", "applied", false);
    }
}
