// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Shared internal/CLI planning API. Saved intent is never an applied acknowledgement. */
final class CodecTuning {
    static final String KEY = "aptx_hq_ttp_delta_ms";
    static final String PROVIDER = "samsung-dzif-calculated-hq-ttp";
    static final List<String> COUPLED = Collections.unmodifiableList(
        Arrays.asList("adaptive/hq", "lossless/hq"));
    static String key(String name) {
        if (Arrays.asList(KEY, "adaptive.hq.ttp_delta_ms", "lossless.hq.ttp_delta_ms").contains(name)) return KEY;
        throw new IllegalArgumentException("Unsupported tuning control: " + name + "; inspect tuning capabilities");
    }
    static String value(String input) {
        ExtensionConfig.choice(KEY, input, "0", "50", "100");
        return input;
    }
    static Map<String, Object> capabilities() {
        return ControlJson.object("schema", 1, "controls", Arrays.asList(
            ControlJson.object("key", KEY, "unit", "ms", "values", Arrays.asList(0, 50, 100),
                "default", 0, "provider", PROVIDER, "coupled_contexts", COUPLED,
                "aliases", Arrays.asList("adaptive.hq.ttp_delta_ms", "lossless.hq.ttp_delta_ms"),
                "activation", "offline_dsp_build_then_fresh_graph", "hot_apply", false,
                "profile", CodecPresentation.SAMSUNG_DZIF)),
            "unsupported", Arrays.asList("LL TTP adjustment", "independent Adaptive/Lossless HQ increments", "receiver buffer depth control"),
            "system_owned", Arrays.asList("codec selection", "sample rate", "bit depth", "preferred transport"),
            "observation", "diagnostics.sh capture; status.sh; diagnostics.sh export",
            "applied", false);
    }
    static Map<String, Object> describe(ExtensionConfig config) {
        return ControlJson.object("schema", 1, "requested", ControlJson.object(KEY, Integer.parseInt(config.value(KEY))),
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
        boolean complete = rate != 0 && ("lossless".equals(codec) ? bits == 16 : bits == 24);
        boolean supported = hq && complete && CodecPresentation.SAMSUNG_DZIF.equals(profile);
        int increment = Integer.parseInt(config.value(KEY));
        return ControlJson.object("schema", 1,
            "context", ControlJson.object("profile", profile, "codec", codec, "mode", mode, "sample_rate_hz", rate, "selection_bits", bits),
            "provider", supported ? PROVIDER : null,
            "requested_ms", supported ? increment : 0,
            "state", !supported ? "unsupported" : increment == 0 ? "stock" : "planned_not_applied",
            "reason", supported ? "Build a verified candidate and measure a fresh DSP graph" : "No TTP provider for this profile/codec/mode",
            "coupled_contexts", supported ? COUPLED : Collections.emptyList(),
            "activation", "offline_dsp_build_then_fresh_graph", "applied", false);
    }
}
