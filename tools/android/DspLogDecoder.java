// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;
import java.util.regex.*;

/** Decode DSP observations only. No desired preferences or capability flags enter this model. */
final class DspLogDecoder {
    static final double FRESH_SECONDS = 3.0;
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
    private double last = -1, encoderTime = -1;
    private long epoch, records, packet = -1;
    private String resetReason = "capture start";
    private void reset(String reason) {
        fields.clear(); packet = -1; encoderTime = -1; epoch++; resetReason = reason;
    }
    void contextChanged(String reason) { reset(reason); }
    private void put(String key, Object value, String unit, String source, double time) {
        if (key.length() > 80 || (!fields.containsKey(key) && fields.size() >= 80)) return;
        fields.put(key, new Field(value, unit, source, time));
        encoderTime = time; // Fresh recognized DSP data, including legacy TTP without R3 packet summaries.
    }
    private static Matcher match(String regex, String text) {
        return PATTERNS.computeIfAbsent(regex, key -> Pattern.compile(key, Pattern.MULTILINE)).matcher(text);
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
    void accept(double time, String text) {
        if (!Double.isFinite(time) || time < 0 || text == null || text.length() > 16384)
            throw new IllegalArgumentException("invalid DSP observation");
        if (last >= 0 && time < last) reset("observation clock moved backwards");
        last = time;
        if (!(text.contains("APTX") || text.contains("aptx") || text.contains("hQLatTarget0") || text.contains("Shizuka TTP") ||
            text.contains("IMCL_PARAM_ID_BT_SIDEBAND_ENCODER_FEEDBACK") || text.contains("COP_SIDEBAND_ID_ENCODER_FEEDBACK"))) return;
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
        Matcher number = match("^Packet- #(\\d+)\\s*$", text);
        if (number.find()) {
            long value;
            try { value = Long.parseLong(number.group(1)); }
            catch (NumberFormatException invalid) { reset("invalid packet counter"); return; }
            if (packet >= 0 && value <= packet) reset("packet counter reset/replay/wrap");
            packet = value;
            put("packet_number", value, "counter", "encoder packet summary", time);
            Matcher state = match("^HQ State- (\\S+)", text);
            if (state.find()) {
                String name = state.group(1);
                put("encoder_state", name, "enum", "encoder packet summary", time);
                Boolean lossless = name.equals("STATE_ENCLEVEL_5_LOSSLESS") ? Boolean.TRUE
                    : name.matches("STATE_ENCLEVEL_[0-4]") ? Boolean.FALSE : null;
                put("encoder_lossless", lossless, "boolean", "encoder state only; not receiver bit-perfect verification", time);
            } else {
                // A newer packet without a state must not refresh an older Lossless decision.
                fields.remove("encoder_state"); fields.remove("encoder_lossless");
            }
            encoderTime = time;
        }
        String summary = "encoder summary";
        integer(text, "^Size- (\\d+)", "packet_payload_bytes", "byte", summary, time);
        integer(text, "^Channels- (\\d+)", "channels", "channel", summary, time);
        decimal(text, "^Interval- ([0-9.]+)", "packet_interval_ms", "ms", summary, time);
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
        return ControlJson.object("schema", 2, "available", encoderTime >= 0 && age >= 0 && age <= FRESH_SECONDS,
            "monotonic_s", encoderTime, "fields", values, "epoch", epoch, "epoch_reason", resetReason,
            "recognized_records", records, "receiver_bit_perfect", "Unverified",
            "clock", "APSS CLOCK_MONOTONIC observation; not DSP generation or receiver playout");
    }
}
