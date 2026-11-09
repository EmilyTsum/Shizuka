// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

public final class DspLogDecoderTest {
    static int assertions;
    static void check(boolean value) { assertions++; if (!value) throw new AssertionError("check " + assertions); }
    @SuppressWarnings("unchecked")
    static Map<String, Object> field(DspLogDecoder decoder, String key, double now) {
        return (Map<String, Object>)((Map<?, ?>)decoder.snapshot(now).get("fields")).get(key);
    }
    static String packet(long number, String state) {
        return "SPF APTX_ADAPTIVE3_ENC: \nPacket- #" + number + "\nHQ State- " + state + "\nSize- 656\nChannels- 2\nInterval- 30.0\nSplit-TX- 1\n";
    }
    public static void main(String[] args) {
        DspLogDecoder d = new DspLogDecoder();
        d.accept(10, packet(1, "STATE_ENCLEVEL_5_LOSSLESS"));
        check(field(d, "encoder_lossless", 10).get("value").equals(true));
        check(d.snapshot(10).get("receiver_bit_perfect").equals("Unverified"));
        check(d.snapshot(10).get("available").equals(true));
        check(field(d, "packet_interval_ms", 10).get("value").equals(30.0));
        d.accept(10.1, "SPF APTX_ADAPTIVE3_ENC:\nqlevel- 5\nrssi- -48.2\nQHS- 1\nDetected 16 bit - 1\nSample rate - 44100\n");
        check(field(d, "qhs", 10.1).get("value").equals(1L));
        check(field(d, "rssi_dbm", 10.1).get("value").equals(-48.2));
        d.accept(10.2, "Value of 'me->profile.hQLatTarget0' used in calcttpAdj calculation is  = 70 ");
        // Wrapper strings without an aptX tag still contain calcttpAdj and must be recognized.
        check(field(d, "hq_target_word", 10.2) != null);
        d.accept(10.3, "CAPI V2 APTX ADAPTIVE ENC Calculated ttpadj: ttpadj microseconds 187000");
        check(field(d, "calculated_ttp_us", 10.3).get("value").equals(187000L));
        d.accept(10.4, "Shizuka TTP HQ requested +100ms; calculated us 287000");
        check(field(d, "ttp_trial_requested_ms", 10.4).get("value").equals(100L));
        check(field(d, "calculated_ttp_us", 10.4).get("value").equals(287000L));
        d.accept(10.5, "SPF APTX_ADAPTIVE3_ENC: CQDDR Status preferredQHS is 0");
        check(field(d, "qhs", 10.5).get("value").equals(1L));
        check(field(d, "preferred_qhs", 10.5).get("value").equals(0L));
        d.accept(10.6, packet(2, "STATE_ENCLEVEL_4"));
        check(field(d, "encoder_lossless", 10.6).get("value").equals(false));
        d.accept(10.7, packet(3, "FUTURE_STATE"));
        check(field(d, "encoder_lossless", 10.7).get("value") == null);
        check(field(d, "qhs", 14).get("fresh").equals(false));
        check(d.snapshot(14).get("available").equals(false));
        d.accept(15, "unrelated logs must not renew any observation");
        check(d.snapshot(15).get("available").equals(false));
        d.accept(16, packet(1, "STATE_ENCLEVEL_3"));
        check(field(d, "qhs", 16) == null); // Counter generation changed.
        d.accept(17, "SPF APTX_ADAPTIVE3_ENC: \nSample rate - 48000\n");
        d.accept(18, "SPF APTX_ADAPTIVE3_ENC: \nSample rate - 96000\n");
        check(field(d, "encoder_lossless", 18) == null);
        d.accept(19, "SPF APTX_ADAPTIVE3_ENC: ENABLE_LOSSLESS_DEBUG - 1");
        check(field(d, "flag.ENABLE_LOSSLESS_DEBUG", 19).get("value").equals(1L));
        check(field(d, "encoder_lossless", 19) == null); // Debug/capability flag is not actual state.
        d.accept(1, packet(1, "STATE_ENCLEVEL_4"));
        check(field(d, "flag.ENABLE_LOSSLESS_DEBUG", 1) == null);
        d.accept(2, "SPF APTX_ADAPTIVE3_ENC: \nPacket- #999999999999999999999999999999999\nHQ State- STATE_ENCLEVEL_5_LOSSLESS\n");
        check(d.snapshot(2).get("available").equals(false));
        d.accept(3, "IMCL_PARAM_ID_BT_SIDEBAND_ENCODER_FEEDBACK - QHS support is 1");
        check(field(d, "feedback_qhs_support", 3).get("value").equals(1L));
        check(field(d, "encoder_lossless", 3) == null);
        d.accept(3.1, packet(1, "STATE_ENCLEVEL_5_LOSSLESS"));
        d.contextChanged("Android context changed");
        check(d.snapshot(3.2).get("available").equals(false));
        check(field(d, "encoder_lossless", 3.2) == null);
        DspLogDecoder legacy = new DspLogDecoder();
        legacy.accept(10, "CAPI V2 APTX_ADAPTIVE_ENC: set-param Wrapper - me->linkState.sampleRate is 96000");
        legacy.accept(20, "CAPI V2 APTX ADAPTIVE ENC Calculated ttpadj: ttpadj microseconds 197000");
        check(legacy.snapshot(20).get("available").equals(true));
        check(field(legacy, "sample_rate_hz", 20).get("fresh").equals(true));
        check(field(legacy, "sample_rate_hz", 20).get("age_seconds").equals(10.0));
        check(field(legacy, "encoder_lossless", 20) == null);
        check(legacy.snapshot(24).get("available").equals(false));
        legacy.accept(25, "Shizuka TTP HQ+50 LL+100 requested; calculated us 179000");
        check(field(legacy, "ttp_hq_trial_requested_ms", 25).get("value").equals(50L));
        check(field(legacy, "ttp_ll_trial_requested_ms", 25).get("value").equals(100L));
        check(field(legacy, "calculated_ttp_us", 25).get("value").equals(179000L));
        check(field(legacy, "ttp_trial_requested_ms", 25) == null);
        for (double bad : new double[] {Double.NaN, Double.POSITIVE_INFINITY, -1}) {
            try { d.accept(bad, "APTX"); throw new AssertionError(); } catch (IllegalArgumentException expected) { assertions++; }
        }
        check(ControlJson.encode(ControlJson.object("x", "\n\"\\", "unknown", null)).contains("\\u000a"));
        System.out.println("DSP decoder: " + assertions + " checks; actual-state Lossless, units, independent flags, expiry and generation isolation passed.");
    }
}
