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
    static String r3Output(int type, long bytes, long nominal) {
        return "SPF APTX_ADAPTIVE3_ENC: Packet type is " + type + ". Packet size is " + bytes + ". Payload size is " + nominal;
    }
    static String legacyOutput(String side, String packet, String cycle, String pointer, String bytes) {
        return "CAPI V2 APTX ADAPTIVE ENC:PROCESS()- dispatching " + side + " Enc pkt #" + packet + " at cycle " + cycle +
            ", output[port]->buf_ptr[0].data_ptr is " + pointer + ", actual_data_len = " + bytes;
    }
    static void outputObservations() {
        final String key = "encoder_output_buffer_bytes";
        DspLogDecoder d = new DspLogDecoder();
        int[] lengths = {348, 656, 140, 152, 560, 760, 960, 348, 980};
        for (int type = 0; type < lengths.length; type++) {
            d.accept(150 + type, r3Output(type, lengths[type] + 8, lengths[type]));
            check(field(d, key, 150 + type).get("value").equals((long)lengths[type] + 8));
            check(field(d, key, 150 + type).get("unit").equals("B"));
            check(field(d, key, 150 + type).get("source").toString().contains("not transmitted throughput"));
            check(field(d, "encoder_frame_bitrate_bps", 150 + type) == null);
        }
        String valid = r3Output(1, 664, 656);
        d.accept(160, valid);
        check(field(d, key, 163).get("fresh").equals(true));
        check(field(d, key, 163.001).get("fresh").equals(false));
        check(d.snapshot(164).get("available").equals(false));
        for (String invalid : new String[] {
                r3Output(1, 665, 656), r3Output(1, 664, 655), r3Output(9, 664, 656),
                r3Output(-1, 664, 656), r3Output(1, 0, 656), r3Output(1, 4097, 4089),
                valid.replace("type is 1", "type is 01"), valid.replace("size is 664", "size is -664"),
                valid.replace("size is 664", "size is 999999999999999999999999"),
                valid.replace("Payload size is 656", "Payload size is NaN"),
                valid.replace("Payload size is 656", "Payload size is 656 trailing"),
                valid.replace(". Payload size is 656", ""), valid + "\n" + valid,
                valid + "\nSPF APTX_ADAPTIVE3_ENC: Packet type is broken",
                "SPF APTX_ADAPTIVE3_ENC: Packet type 1, bad size 665 (should be 656)"}) {
            d.accept(170, valid); d.accept(170.1, invalid);
            check(field(d, key, 170.1) == null); // Malformed latest tuple cannot leave old bytes fresh.
        }
        String left = legacyOutput("Left", "12", "52", "0x1234abcd", "336");
        String right = legacyOutput("Right", "12", "53", "0x00000000", "336");
        d.accept(180, left);
        check(field(d, key, 180).get("value").equals(336L));
        check(field(d, key, 180).get("source").toString().contains("Legacy Left"));
        d.accept(180.1, right);
        check(field(d, key, 180.1).get("source").toString().contains("Legacy Right"));
        check(!ControlJson.encode(d.snapshot(180.1)).contains("0x00000000"));
        check(field(d, "encoder_state", 180.1) == null);
        d.accept(180.2, legacyOutput("Left", "4294967295", "4294967295", "0x0", "4096"));
        check(field(d, key, 180.2).get("value").equals(4096L)); // Parser bound, not advertised codec maximum.
        for (String invalid : new String[] {
                legacyOutput("Middle", "12", "52", "0x1234abcd", "336"),
                legacyOutput("Left", "4294967296", "52", "0x1234abcd", "336"),
                legacyOutput("Left", "12", "4294967296", "0x1234abcd", "336"),
                legacyOutput("Left", "99999999999999999999999", "52", "0x1234abcd", "336"),
                legacyOutput("Left", "12", "52", "0x123456789", "336"),
                legacyOutput("Left", "12", "52", "pointer", "336"),
                legacyOutput("Left", "12", "52", "0x1234abcd", "0"),
                legacyOutput("Left", "12", "52", "0x1234abcd", "7"),
                legacyOutput("Left", "12", "52", "0x1234abcd", "4097"),
                legacyOutput("Left", "12", "52", "0x1234abcd", "-1"),
                legacyOutput("Left", "12", "52", "0x1234abcd", "0336"),
                legacyOutput("Left", "12", "52", "0x1234abcd", "99999999999999999999999"),
                left + " trailing", left + "\n" + right, left + "\n" + valid,
                left.replace("actual_data_len = 336", "actual_data_len = bad")}) {
            d.accept(190, valid); d.accept(190.1, invalid);
            check(field(d, key, 190.1) == null);
        }
        d.accept(200, valid); d.contextChanged("route/codec context changed");
        check(field(d, key, 200.1) == null);
        d.accept(201, valid); d.accept(201.1, "SPF APTX_ADAPTIVE3_ENC: Param INIT: init");
        check(field(d, key, 201.1) == null);
        d.accept(202, "SPF APTX_ADAPTIVE3_ENC: Sample rate - 44100"); d.accept(202.1, valid);
        d.accept(202.2, "SPF APTX_ADAPTIVE3_ENC: Sample rate - 96000");
        check(field(d, key, 202.2) == null);
        d.accept(203, valid); d.accept(202, "APTX unrelated");
        check(field(d, key, 202) == null);
        d.accept(204, valid); d.accept(204.1, packet(1, "STATE_ENCLEVEL_4"));
        check(field(d, key, 204.1) == null); // A new selected budget is not completed output.
        d.accept(205, valid); d.accept(205.1, "SPF APTX_ADAPTIVE3_ENC: Packet- #-1");
        check(field(d, key, 205.1) == null);
        d.accept(206, valid); d.accept(206.1, "unrelated encoder: Packet type is 1. Packet size is 0. Payload size is 0");
        check(field(d, key, 206.1).get("monotonic_s").equals(206.0));
    }
    public static void main(String[] args) {
        DspLogDecoder d = new DspLogDecoder();
        d.accept(10, packet(1, "STATE_ENCLEVEL_5_LOSSLESS"));
        check(field(d, "encoder_state", 10).get("value").equals("STATE_ENCLEVEL_5_LOSSLESS"));
        check(field(d, "encoder_level", 10).get("value").equals(5L));
        check(field(d, "encoder_lossless", 10).get("value").equals(true));
        check(d.snapshot(10).get("receiver_bit_perfect").equals("Unverified"));
        check(d.snapshot(10).get("available").equals(true));
        check(field(d, "packet_interval_ms", 10).get("value").equals(30.0));
        check(field(d, "encoder_frame_bitrate_bps", 10).get("value").equals(349867L));
        check(field(d, "packet_payload_bytes", 10).get("unit").equals("byte/channel"));
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
        check(field(d, "encoder_state", 10.6).get("value").equals("STATE_ENCLEVEL_4"));
        check(field(d, "encoder_level", 10.6).get("value").equals(4L));
        check(field(d, "quality_level", 10.6).get("value").equals(5L)); // Separate observed log, not an ENCLEVEL mapping.
        List<?> changes = (List<?>)d.snapshot(10.6).get("encoder_transitions");
        check(changes.size() == 2);
        check(((Map<?, ?>)changes.get(1)).get("previous").equals("STATE_ENCLEVEL_5_LOSSLESS"));
        check(((Map<?, ?>)changes.get(1)).get("state").equals("STATE_ENCLEVEL_4"));
        check(((Map<?, ?>)changes.get(1)).get("monotonic_s").equals(10.6));
        check(field(d, "encoder_lossless", 10.6).get("value").equals(false));
        d.accept(10.7, packet(3, "FUTURE_STATE"));
        check(field(d, "encoder_level", 10.7) == null);
        check(field(d, "encoder_lossless", 10.7).get("value") == null);
        check(field(d, "qhs", 14).get("fresh").equals(false));
        check(field(d, "encoder_state", 14).get("fresh").equals(false));
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
        check(((List<?>)d.snapshot(3.2).get("encoder_transitions")).isEmpty());
        check(field(d, "encoder_level", 3.2) == null);
        check(d.snapshot(3.2).get("available").equals(false));
        check(field(d, "encoder_lossless", 3.2) == null);
        DspLogDecoder legacy = new DspLogDecoder();
        legacy.accept(10, "CAPI V2 APTX_ADAPTIVE_ENC: set-param Wrapper - me->linkState.sampleRate is 96000");
        legacy.accept(20, "CAPI V2 APTX ADAPTIVE ENC Calculated ttpadj: ttpadj microseconds 197000");
        check(legacy.snapshot(20).get("available").equals(true));
        check(field(legacy, "sample_rate_hz", 20).get("fresh").equals(true));
        check(field(legacy, "sample_rate_hz", 20).get("age_seconds").equals(10.0));
        check(field(legacy, "encoder_lossless", 20) == null);
        check(field(legacy, "encoder_level", 20) == null);
        check(legacy.snapshot(24).get("available").equals(false));
        legacy.accept(24.1, "IMCL_PARAM_ID_BT_BIT_RATE_LEVEL_ENCODER_FEEDBACK br_level =  5");
        check(field(legacy, "feedback_bitrate_level", 24.1).get("value").equals(5L));
        check(field(legacy, "encoder_level", 24.1) == null);
        check(field(legacy, "encoder_state", 24.1) == null);
        check(field(legacy, "encoder_lossless", 24.1) == null);
        legacy.accept(24.2, "CAPI V2 APTX ADAPTIVE ENC: PROCESS(): aptXEncode_SetBitRate() set to desired value  650000 kbps, System mode is 3, Latency state is 3");
        check(field(legacy, "desired_encoder_bitrate_raw", 24.2).get("value").equals(650000L));
        check(field(legacy, "desired_encoder_bitrate_raw", 24.2).get("unit").equals("raw log value"));
        check(field(legacy, "feedback_bitrate_level", 27.3).get("fresh").equals(false));
        check(field(legacy, "encoder_lossless", 24.2) == null);
        legacy.accept(25, "Shizuka TTP HQ+50 LL+100 requested; calculated us 179000");
        check(field(legacy, "ttp_hq_trial_requested_ms", 25).get("value").equals(50L));
        check(field(legacy, "ttp_ll_trial_requested_ms", 25).get("value").equals(100L));
        check(field(legacy, "calculated_ttp_us", 25).get("value").equals(179000L));
        check(field(legacy, "ttp_trial_requested_ms", 25) == null);
        DspLogDecoder rejected = new DspLogDecoder();
        rejected.accept(30, packet(1, "STATE_ENCLEVEL_5_LOSSLESS"));
        rejected.accept(30.1, "ADSP:[AS]: Segment [0] failed to read or streamed SigVerify for aptx_adaptive_enc_module.so.1");
        check(rejected.snapshot(30.1).get("available").equals(false));
        check(field(rejected, "encoder_lossless", 30.1) == null);
        Map<?, ?> loader = (Map<?, ?>)rejected.snapshot(30.1).get("loader");
        check(loader.get("code").equals("signature_verification_failed"));
        rejected.accept(30.2, "ADSP:[AS]: Error: dlopen_ex failed for aptx_adaptive_enc_module.so.1 (flags 2)");
        check(((Map<?, ?>)rejected.snapshot(30.2).get("loader")).get("code").equals("signature_verification_failed"));
        rejected.contextChanged("media route changed to speaker");
        check(((Map<?, ?>)rejected.snapshot(30.3).get("loader")).get("code").equals("signature_verification_failed"));
        check(rejected.snapshot(30.3).get("available").equals(false));
        check(((Map<?, ?>)rejected.snapshot(34).get("loader")).get("fresh").equals(false));
        rejected.accept(35, "APTX ENC INIT: starting");
        check(!rejected.snapshot(35).containsKey("loader"));
        rejected.accept(36, packet(1, "STATE_ENCLEVEL_4"));
        rejected.accept(36.1, "ADSP:[AS]: Segment [0] failed to read or streamed SigVerify for unrelated.so.1");
        check(rejected.snapshot(36.1).get("available").equals(true));
        check(!rejected.snapshot(36.1).containsKey("loader"));
        DspLogDecoder exact = new DspLogDecoder();
        for (int level = 0; level < 5; level++) {
            exact.accept(40 + level, packet(level + 1, "STATE_ENCLEVEL_" + level));
            check(field(exact, "encoder_level", 40 + level).get("value").equals((long)level));
            check(field(exact, "encoder_state", 40 + level).get("value").equals("STATE_ENCLEVEL_" + level));
        }
        exact.accept(46, "SPF APTX_ADAPTIVE3_ENC:\nPacket- #8\nSize- 656\nqlevel- 5\n");
        check(field(exact, "encoder_level", 46) == null);
        check(field(exact, "encoder_state", 46) == null);
        check(field(exact, "quality_level", 46).get("value").equals(5L));
        DspLogDecoder timeline = new DspLogDecoder();
        for (int i = 0; i < 20; i++) timeline.accept(100 + i, packet(i + 1, "STATE_ENCLEVEL_" + (i % 5)));
        List<?> bounded = (List<?>)timeline.snapshot(120).get("encoder_transitions");
        check(bounded.size() == 8);
        check(((Map<?, ?>)bounded.get(0)).get("monotonic_s").equals(112.0));
        timeline.accept(121, packet(21, "STATE_ENCLEVEL_4"));
        check(((List<?>)timeline.snapshot(121).get("encoder_transitions")).size() == 8);
        check(((Map<?, ?>)((List<?>)timeline.snapshot(121).get("encoder_transitions")).get(7)).get("monotonic_s").equals(119.0));
        timeline.accept(122, "APTX ENC INIT: init");
        check(((List<?>)timeline.snapshot(122).get("encoder_transitions")).isEmpty());
        DspLogDecoder rate = new DspLogDecoder();
        String frame = "SPF APTX_ADAPTIVE3_ENC:\nPacket- #1\nHQ State- STATE_ENCLEVEL_5_LOSSLESS\nSize- 960\nChannels- 2\nInterval- 15.0\n";
        rate.accept(130, frame);
        check(field(rate, "encoder_frame_bitrate_bps", 130).get("value").equals(1024000L));
        check(field(rate, "encoder_frame_bitrate_bps", 134).get("fresh").equals(false));
        rate.accept(131, frame.replace("#1", "#2").replace("Size- 960\n", ""));
        check(field(rate, "encoder_frame_bitrate_bps", 131) == null);
        check(field(rate, "packet_payload_bytes", 131) == null);
        rate.accept(132, frame.replace("#1", "#3").replace("Interval- 15.0\n", ""));
        check(field(rate, "encoder_frame_bitrate_bps", 132) == null);
        check(field(rate, "packet_interval_ms", 132) == null);
        rate.accept(133, frame.replace("#1", "#4").replace("960", "760").replace("15.0", "50.0"));
        check(field(rate, "encoder_frame_bitrate_bps", 133).get("value").equals(243200L));
        rate.accept(134, frame.replace("#1", "#5").replace("Channels- 2", "Channels- 1"));
        check(field(rate, "encoder_frame_bitrate_bps", 134).get("value").equals(512000L));
        for (String malformed : new String[] {
                frame.replace("Size- 960", "Size- 0"), frame.replace("Size- 960", "Size- 4194305"),
                frame.replace("Size- 960", "Size- 9999999999999999999999999"),
                frame.replace("Channels- 2", "Channels- 0"), frame.replace("Channels- 2", "Channels- 3"),
                frame.replace("Interval- 15.0", "Interval- 0"), frame.replace("Interval- 15.0", "Interval- NaN"),
                frame.replace("Interval- 15.0", "Interval- 0.1"), frame.replace("Interval- 15.0", "Interval- 121"),
                frame.replace("Interval- 15.0", "Interval- 1.2.3"),
                frame.replace("Size- 960", "Size- 960\nSize- 480"),
                frame.replace("Size- 960", "Size- 960\nSize- broken"),
                frame.replace("Channels- 2", "Channels- 2\nChannels- broken"),
                frame.replace("Interval- 15.0", "Interval- 15.0\nInterval- 30.0"),
                frame.replace("Interval- 15.0", "Interval- 15.0\nInterval- broken"),
                frame.replace("Packet- #1", "Packet- #1\nPacket- #-1"),
                frame + frame.replace("#1", "#2"),
                frame.replace("SPF APTX_ADAPTIVE3_ENC", "APTX unrelated")}) {
            rate.contextChanged("new test context"); rate.accept(140, malformed);
            check(field(rate, "encoder_frame_bitrate_bps", 140) == null);
        }
        rate.accept(141, "CAPI V2 APTX ADAPTIVE ENC: PROCESS(): aptXEncode_SetBitRate() set to desired value 650000 kbps");
        check(field(rate, "encoder_frame_bitrate_bps", 141) == null);
        rate.accept(141.1, frame);
        check(field(rate, "encoder_frame_bitrate_bps", 141.1) != null);
        rate.accept(141.2, "SPF APTX_ADAPTIVE3_ENC:\nPacket- #-1\n");
        check(field(rate, "encoder_frame_bitrate_bps", 141.2) == null);
        rate.contextChanged("new stream");
        check(field(rate, "encoder_frame_bitrate_bps", 142) == null);
        outputObservations();
        for (double bad : new double[] {Double.NaN, Double.POSITIVE_INFINITY, -1}) {
            try { d.accept(bad, "APTX"); throw new AssertionError(); } catch (IllegalArgumentException expected) { assertions++; }
        }
        check(ControlJson.encode(ControlJson.object("x", "\n\"\\", "unknown", null)).contains("\\u000a"));
        System.out.println("DSP decoder: " + assertions + " checks; actual-state Lossless, units, independent flags, expiry and generation isolation passed.");
    }
}
