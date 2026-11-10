// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;
import java.util.regex.*;

/** Decode DSP observations only. No desired preferences or capability flags enter this model. */
final class DspLogDecoder {
    static final double FRESH_SECONDS = 3.0;
    // Supported diagnostic tuples, not a codec maximum or RF byte count.
    private static final long MAX_OUTPUT_BUFFER_BYTES = 4096;
    private static final long[] R3_PAYLOAD_BYTES = {348, 656, 140, 152, 560, 760, 960, 348, 980};
    private static final String OUTPUT_BYTES = "encoder_output_buffer_bytes";
    private static final String R3_OUTPUT = "SPF APTX_ADAPTIVE3_ENC: Packet type";
    private static final String LEGACY_OUTPUT = "CAPI V2 APTX ADAPTIVE ENC:PROCESS()- dispatching ";
    private static final Map<String, Pattern> PATTERNS = new java.util.concurrent.ConcurrentHashMap<>();
    private static final class Field {
        final Object value;
        final String unit, source;
        final double time;
        Field(Object value, String unit, String source, double time) {
            this.value = value; this.unit = unit; this.source = source; this.time = time;
        }
    }
    private final Map<String, Field> fields = new LinkedHashMap<>();
    private final List<Map<String, Object>> transitions = new ArrayList<>();
    private double last = -1, encoderTime = -1;
    private long epoch, records, packet = -1;
    private String resetReason = "capture start";
    private double loadErrorTime = -1;
    private String loadError;
    private void reset(String reason) {
        fields.clear(); transitions.clear(); packet = -1; encoderTime = -1; epoch++; resetReason = reason;
        loadErrorTime = -1; loadError = null;
    }
    void contextChanged(String reason) {
        // Route fallback is often caused by the load failure itself. Preserve the
        // independent loader event until it expires or a new encoder initializes.
        String previousError = loadError;
        double previousTime = loadErrorTime;
        reset(reason);
        loadError = previousError; loadErrorTime = previousTime;
    }
    private void put(String key, Object value, String unit, String source, double time) {
        if (key.length() > 80 || (!fields.containsKey(key) && fields.size() >= 80)) return;
        fields.put(key, new Field(value, unit, source, time));
        encoderTime = time; // Fresh recognized DSP data, including legacy TTP without R3 packet summaries.
    }
    private static Matcher match(String regex, String text) {
        return PATTERNS.computeIfAbsent(regex, key -> Pattern.compile(key, Pattern.MULTILINE)).matcher(text);
    }
    private static boolean singleLine(String regex, String text) {
        Matcher found = match(regex, text);
        return found.find() && !found.find();
    }
    private void integer(String text, String regex, String key, String unit, String source, double time) {
        Matcher m = match(regex, text);
        if (m.find()) try { put(key, Long.parseLong(m.group(1)), unit, source, time); }
        catch (NumberFormatException ignored) { /* Malformed or overflowing values are not observations. */ }
    }
    private void decimal(String text, String regex, String key, String unit, String source, double time) {
        Matcher m = match(regex, text);
        if (m.find()) try {
            double value = Double.parseDouble(m.group(1));
            if (Double.isFinite(value)) put(key, value, unit, source, time);
        } catch (NumberFormatException ignored) {}
    }
    private void outputBuffer(String text, double time) {
        boolean r3 = text.contains(R3_OUTPUT), legacy = text.contains(LEGACY_OUTPUT);
        if (!r3 && !legacy) return;
        fields.remove(OUTPUT_BYTES);
        // One whole tuple in one DSP record. Bad-size and mixed/duplicate lines
        // invalidate an older observation instead of silently renewing it.
        if (r3 == legacy || !singleLine("^" + Pattern.quote(r3 ? R3_OUTPUT : LEGACY_OUTPUT) + "[^\\r\\n]*$", text)) return;
        String source;
        long bytes;
        try {
            if (r3) {
                Matcher tuple = match("^" + Pattern.quote(R3_OUTPUT) +
                    " is (0|[1-9][0-9]*)\\. Packet size is (0|[1-9][0-9]*)\\. Payload size is (0|[1-9][0-9]*)[ \\t]*$", text);
                if (!tuple.find()) return;
                long type = Long.parseLong(tuple.group(1));
                bytes = Long.parseLong(tuple.group(2));
                long nominal = Long.parseLong(tuple.group(3));
                if (type >= R3_PAYLOAD_BYTES.length || nominal != R3_PAYLOAD_BYTES[(int) type] || bytes != nominal + 8) return;
                source = "R3 end-of-process actual_data_len; includes common wrapper; not transmitted throughput";
            } else {
                Matcher tuple = match("^" + Pattern.quote(LEGACY_OUTPUT) +
                    "(Left|Right) Enc pkt #(0|[1-9][0-9]*) at cycle (0|[1-9][0-9]*), " +
                    Pattern.quote("output[port]->buf_ptr[0].data_ptr is ") +
                    "0x[0-9a-fA-F]{1,8}, actual_data_len = (0|[1-9][0-9]*)[ \\t]*$", text);
                if (!tuple.find()) return;
                long packetNumber = Long.parseLong(tuple.group(2)), cycle = Long.parseLong(tuple.group(3));
                if (packetNumber > 0xffffffffL || cycle > 0xffffffffL) return;
                bytes = Long.parseLong(tuple.group(4));
                source = "Legacy " + tuple.group(1) + " dispatch actual_data_len; includes common wrapper; not transmitted throughput";
            }
        } catch (NumberFormatException overflow) { return; }
        if (bytes >= 8 && bytes <= MAX_OUTPUT_BUFFER_BYTES) put(OUTPUT_BYTES, bytes, "B", source, time);
    }
    void accept(double time, String text) {
        if (!Double.isFinite(time) || time < 0 || text == null || text.length() > 16384)
            throw new IllegalArgumentException("invalid DSP observation");
        if (last >= 0 && time < last) reset("observation clock moved backwards");
        last = time;
        // A rejected library is a loader observation, never proof of a running encoder.
        // Match the module name so another DSP library cannot invalidate this encoder.
        if (text.contains("aptx_adaptive_enc_module.so.1") &&
            (text.contains("SigVerify") || text.contains("dlopen_ex failed"))) {
            boolean signature = text.contains("SigVerify");
            if (signature || !"signature_verification_failed".equals(loadError)) {
                reset("encoder module load failed");
                loadError = signature ? "signature_verification_failed" : "module_load_failed";
            }
            loadErrorTime = time;
            return;
        }
        if (!(text.contains("APTX") || text.contains("aptx") || text.contains("hQLatTarget0") || text.contains("Shizuka TTP") ||
            text.contains("IMCL_PARAM_ID_BT_SIDEBAND_ENCODER_FEEDBACK") || text.contains("COP_SIDEBAND_ID_ENCODER_FEEDBACK") ||
            text.contains("IMCL_PARAM_ID_BT_BIT_RATE_LEVEL_ENCODER_FEEDBACK"))) return;
        records++;
        if (text.contains("Param INIT:") || text.contains("ENC INIT:") || text.contains("ENC INIT DONE")) reset("encoder initialization");
        Matcher rate = match("(?:Sample rate - |linkState\\.sampleRate is |sample rate is )(\\d+)", text);
        if (rate.find()) {
            long value;
            try { value = Long.parseLong(rate.group(1)); }
            catch (NumberFormatException invalid) { reset("invalid sample rate"); return; }
            Field previous = fields.get("sample_rate_hz");
            if (previous != null && !previous.value.equals(value)) reset("sample rate changed");
            if (value == 44100 || value == 48000 || value == 96000)
                put("sample_rate_hz", value, "Hz", "DSP sample-rate log", time);
        }
        boolean packetSummary = false;
        if (text.contains("Packet- #"))
            for (String key : new String[] {"packet_payload_bytes", "channels", "packet_interval_ms", "encoder_frame_bitrate_bps", OUTPUT_BYTES}) fields.remove(key);
        Matcher number = match("^Packet- #(\\d+)\\s*$", text);
        if (number.find()) {
            long value;
            try { value = Long.parseLong(number.group(1)); }
            catch (NumberFormatException invalid) { reset("invalid packet counter"); return; }
            if (packet >= 0 && value <= packet) reset("packet counter reset/replay/wrap");
            packet = value;
            packetSummary = !number.find() && singleLine("^Packet-[^\\r\\n]*$", text);
            put("packet_number", value, "counter", "encoder packet summary", time);
            Matcher state = match("^HQ State- (\\S+)", text);
            if (state.find()) {
                String name = state.group(1);
                if (name.matches("STATE_ENCLEVEL_[0-4]|STATE_ENCLEVEL_5_LOSSLESS")) {
                    Field previous = fields.get("encoder_state");
                    String before = previous != null && previous.value instanceof String ? (String)previous.value : null;
                    if (before != null && !before.matches("STATE_ENCLEVEL_[0-4]|STATE_ENCLEVEL_5_LOSSLESS")) before = null;
                    if (!name.equals(before)) {
                        if (transitions.size() == 8) transitions.remove(0);
                        transitions.add(ControlJson.object("state", name, "previous", before,
                            "monotonic_s", time, "source", "encoder packet summary"));
                    }
                }
                put("encoder_state", name, "enum", "encoder packet summary", time);
                if (name.matches("STATE_ENCLEVEL_[0-4]"))
                    put("encoder_level", (long)(name.charAt(name.length() - 1) - '0'), "level",
                        "numeric suffix of the exact encoder-state log; not qlevel or feedback quality", time);
                else if (name.equals("STATE_ENCLEVEL_5_LOSSLESS"))
                    put("encoder_level", 5L, "level",
                        "numeric suffix of the exact encoder-state log; not receiver verification", time);
                else fields.remove("encoder_level");
                Boolean lossless = name.equals("STATE_ENCLEVEL_5_LOSSLESS") ? Boolean.TRUE
                    : name.matches("STATE_ENCLEVEL_[0-4]") ? Boolean.FALSE : null;
                put("encoder_lossless", lossless, "boolean", "encoder state only; not receiver bit-perfect verification", time);
            } else {
                // A newer packet without a state must not refresh an older Lossless decision.
                fields.remove("encoder_state"); fields.remove("encoder_level"); fields.remove("encoder_lossless");
            }
            encoderTime = time;
        }
        String summary = "encoder summary";
        integer(text, "^Size- (\\d+)", "packet_payload_bytes", "byte/channel", "R3 selected frame-size budget divided by channel count; not measured radio payload", time);
        integer(text, "^Channels- (\\d+)", "channels", "channel", summary, time);
        decimal(text, "^Interval- ([0-9.]+)", "packet_interval_ms", "ms", summary, time);
        if (packetSummary && text.contains("SPF APTX_ADAPTIVE3_ENC") &&
                singleLine("^Size- [0-9]+[ \\t]*$", text) && singleLine("^Channels- [0-9]+[ \\t]*$", text) &&
                singleLine("^Interval- [0-9]+(?:\\.[0-9]+)?[ \\t]*$", text) &&
                singleLine("^Size-[^\\r\\n]*$", text) && singleLine("^Channels-[^\\r\\n]*$", text) && singleLine("^Interval-[^\\r\\n]*$", text)) {
            Field size = fields.get("packet_payload_bytes"), channels = fields.get("channels"), interval = fields.get("packet_interval_ms");
            if (size != null && channels != null && interval != null) {
                long bytes = ((Number)size.value).longValue(), count = ((Number)channels.value).longValue();
                double milliseconds = ((Number)interval.value).doubleValue();
                double bitrate = bytes * 8000.0 * count / milliseconds;
                if (bytes > 0 && bytes <= 4 * 1024 * 1024 && count >= 1 && count <= 2 && milliseconds >= 1 && milliseconds <= 120 &&
                        Double.isFinite(bitrate) && bitrate > 0 && bitrate <= 20_000_000)
                    put("encoder_frame_bitrate_bps", Math.round(bitrate), "bit/s",
                        "R3 frame-budget estimate: Size * Channels * 8000 / Interval(ms), same summary; before output completion, not transmitted/radio bitrate", time);
            }
        }
        integer(text, "^Split-TX- (-?\\d+)", "split_tx", "raw flag", summary, time);
        integer(text, "^qlevel- (-?\\d+)", "quality_level", "level", summary, time);
        integer(text, "^raw rssi- (-?\\d+)", "summary_raw_rssi", "raw value", "summary raw field; not R3 feedback RSSI", time);
        decimal(text, "^rssi- (-?[0-9.]+)", "rssi_dbm", "dBm", summary, time);
        String[][] flags = {{"BT Links", "bt_links"}, {"BLE Links", "ble_links"},
            {"Wifi 2.4 GHz", "wifi_24ghz"}, {"BLE Scan", "ble_scan"}, {"Page Scan", "page_scan"},
            {"QHS", "qhs"}, {"Audio bits/sample", "audio_bits_raw"}, {"Detected 16 bit", "detected_16bit"}};
        // Existing logs use both "QHS- 1" and "Audio bits/sample - 0" spacing.
        for (String[] flag : flags)
            integer(text, "^" + Pattern.quote(flag[0]) + "\\s*-\\s*(-?\\d+)", flag[1], "raw flag", summary, time);
        integer(text, "quality level received is (-?\\d+)", "feedback_quality_level", "level", "IMCL received quality before override/clamp", time);
        integer(text, "IMCL_PARAM_ID_BT_BIT_RATE_LEVEL_ENCODER_FEEDBACK\\s+br_level\\s*=\\s*(-?\\d+)",
            "feedback_bitrate_level", "level", "IMCL bitrate-level feedback; not R3 ENCLEVEL or encoder state", time);
        integer(text, "aptXEncode_SetBitRate\\(\\) set to desired value\\s+(\\d+)\\s+kbps", "desired_encoder_bitrate_raw", "raw log value",
            "legacy encoder requested bitrate; log prints kbps without a verified conversion, not measured transmission", time);
        integer(text, "CQDDR Status preferredQHS is (-?\\d+)", "preferred_qhs", "raw flag", "IMCL CQDDR preference; not actual QHS state", time);
        decimal(text, "RSSI value after applying a smoothing filter is\\s+(-?[0-9.]+)", "feedback_rssi_dbm", "dBm", "IMCL filtered RSSI", time);
        integer(text, "the raw RSSI is\\s+(-?\\d+)", "feedback_raw_rssi", "dBm", "IMCL received RSSI; distinct from summary raw field", time);
        integer(text, "Number of BT links is\\s+(\\d+)", "feedback_bt_links", "count", "IMCL received link count", time);
        integer(text, "Wi-fi 2.4GHz is\\s+(\\d+)", "feedback_wifi_24ghz", "raw flag", "IMCL Wi-Fi activity", time);
        integer(text, "BLE scan is\\s+(\\d+)", "feedback_ble_scan", "raw flag", "IMCL BLE scan", time);
        integer(text, "Page scan is\\s+(\\d+)", "feedback_page_scan", "raw flag", "IMCL page scan", time);
        integer(text, "QHS support is\\s+(\\d+)", "feedback_qhs_support", "raw flag", "IMCL QHS support flag; not RF state", time);
        integer(text, "16-bit audio flag is\\s+(\\d+)", "feedback_detected_16bit", "raw flag", "IMCL audio flag", time);
        integer(text, "System mode is (-?\\d+)", "system_mode", "raw enum", "DSP mode log", time);
        integer(text, "Latency state is (-?\\d+)", "latency_state", "raw enum", "DSP latency state log", time);
        integer(text, "Next State is (-?\\d+)", "next_latency_state", "raw enum", "decideSystemLatency", time);
        integer(text, "hQLatTarget0.*?=\\s*(-?\\d+)", "hq_target_word", "4000 us units on DZIF", "calcTTPAdj target log; not receiver delay", time);
        integer(text, "ttpadj microseconds (-?\\d+)", "calculated_ttp_us", "us", "calcTTPAdj before final state term/packing", time);
        integer(text, "Shizuka TTP HQ requested \\+(\\d+)ms", "ttp_trial_requested_ms", "ms", "DSP trial image request; ceiling guard may decline increment", time);
        integer(text, "Shizuka TTP HQ requested \\+(\\d+)ms", "ttp_hq_trial_requested_ms", "ms", "DSP HQ trial request; not applied increment proof", time);
        integer(text, "Shizuka TTP HQ requested.*?calculated us (-?\\d+)", "calculated_ttp_us", "us", "post-trial-guard calculation before final state term/packing", time);
        if (text.contains("Shizuka TTP HQ+") && text.contains(" LL+")) {
            fields.remove("ttp_trial_requested_ms");
            integer(text, "Shizuka TTP HQ\\+(\\d+) LL\\+\\d+ requested", "ttp_hq_trial_requested_ms", "ms", "DSP HQ request; not applied increment proof", time);
            integer(text, "Shizuka TTP HQ\\+\\d+ LL\\+(\\d+) requested", "ttp_ll_trial_requested_ms", "ms", "DSP Adaptive LL request; not applied increment proof", time);
            integer(text, "Shizuka TTP HQ\\+.*?calculated us (-?\\d+)", "calculated_ttp_us", "us", "post-trial-guard calculation before final state term/packing", time);
        }
        outputBuffer(text, time);
        Matcher debug = match("\\b(ENABLE_[A-Z0-9_]+|SIMULATE_FORCE_LOSSLESS) - (-?\\d+)", text);
        while (debug.find()) try {
            put("flag." + debug.group(1), Long.parseLong(debug.group(2)), "raw flag", "DSP debug configuration log", time);
        } catch (NumberFormatException ignored) {}
    }
    Map<String, Object> snapshot(double now) {
        if (!Double.isFinite(now) || now < 0) throw new IllegalArgumentException("invalid observation clock");
        Map<String, Object> values = new LinkedHashMap<>();
        for (Map.Entry<String, Field> item : fields.entrySet()) {
            Field f = item.getValue();
            double age = now - f.time;
            boolean epochField = item.getKey().equals("sample_rate_hz");
            values.put(item.getKey(), ControlJson.object("value", f.value, "unit", f.unit, "source", f.source,
                "monotonic_s", f.time, "age_seconds", age, "scope", epochField ? "encoder_epoch" : "dynamic",
                "fresh", age >= 0 && (epochField || age <= FRESH_SECONDS)));
        }
        double age = now - encoderTime;
        Map<String, Object> result = ControlJson.object("schema", 2, "available", encoderTime >= 0 && age >= 0 && age <= FRESH_SECONDS,
            "monotonic_s", encoderTime, "fields", values, "epoch", epoch, "epoch_reason", resetReason,
            "recognized_records", records, "receiver_bit_perfect", "Unverified",
            "clock", "APSS CLOCK_MONOTONIC observation; not DSP generation or receiver playout");
        List<Map<String, Object>> observed = new ArrayList<>();
        for (Map<String, Object> transition : transitions) {
            Map<String, Object> copy = new LinkedHashMap<>(transition);
            copy.put("age_seconds", now - ((Number)transition.get("monotonic_s")).doubleValue());
            observed.add(copy);
        }
        result.put("encoder_transitions", observed);
        if (loadError != null) result.put("loader", ControlJson.object("code", loadError,
            "message", "signature_verification_failed".equals(loadError)
                ? "The DSP rejected the modified encoder signature. Restore the signed stock image."
                : "The DSP could not load the Adaptive encoder module.",
            "monotonic_s", loadErrorTime, "age_seconds", now - loadErrorTime,
            "fresh", now >= loadErrorTime && now - loadErrorTime <= FRESH_SECONDS,
            "source", "DSP dynamic-loader log"));
        return result;
    }
}
