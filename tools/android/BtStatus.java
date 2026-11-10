// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.content.Context;
import android.media.AudioManager;
import android.os.Looper;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;
import org.json.*;

/** A read-only snapshot. Never changes connection policy, routing or codec preferences. */
public class BtStatus {
    static Context context;
    static BluetoothAdapter adapter;
    static BluetoothProfiles profiles;
    static final JSONArray errors = new JSONArray();
    static boolean finished;
    static Path moduleRoot;
    static final Path DATA = Paths.get("/data/adb/shizuka");
    static final Path LEGACY_DATA = Paths.get("/data/adb/s25_bluetooth");
    static Object call(Object target, String name, Class<?>[] types, Object... args)
        throws Exception {
        return BluetoothRuntime.call(target, name, types, args);
    }
    static Object get(Object target, String name) throws Exception {
        return call(target, name, new Class<?>[] {});
    }
    static String read(String path) throws Exception {
        Path p = Paths.get(path);
        if (!Files.isRegularFile(p) || Files.size(p) > 65536)
            return "";
        return new String(Files.readAllBytes(p), StandardCharsets.UTF_8).trim();
    }
    static synchronized void error(String scope, Exception e) {
        errors.put(scope + ": " + e.getClass().getSimpleName());
    }
    static JSONArray numericFlags(Object codec, String prefix, int mask) throws Exception {
        TreeSet<Integer> values = new TreeSet<>();
        for (Field f : codec.getClass().getFields()) {
            String name = f.getName();
            if (!Modifier.isStatic(f.getModifiers()) || f.getType() != int.class ||
                !name.startsWith(prefix))
                continue;
            String suffix = name.substring(prefix.length());
            if (!suffix.matches("[0-9]+"))
                continue;
            int flag = f.getInt(null);
            if (flag != 0 && (mask & flag) == flag)
                values.add(Integer.parseInt(suffix));
        }
        return new JSONArray(values);
    }
    static String deviceProfile() throws Exception {
        return moduleRoot == null ? "" : read(moduleRoot.resolve("device-profile").toString());
    }
    static JSONObject codec(Object c, boolean le) throws Exception {
        JSONObject j = new JSONObject();
        if (c == null)
            return j.put("available", false);
        j.put("available", true);
        int type = ((Number)get(c, "getCodecType")).intValue();
        j.put("type", type);
        String profile = deviceProfile();
        String name = null;
        try {
            name = (String)get(c, "getCodecName");
        } catch (UnsupportedOperationException e) {
            Matcher match = Pattern.compile("codecName[:=]([^,}\\]]+)").matcher(c.toString());
            if (match.find()) name = match.group(1).trim();
        }
        j.put("lossless_selection", CodecPresentation.losslessSelection(profile, le, type));
        j.put("name", CodecPresentation.name(profile, le, type, name));
        int rate = ((Number)get(c, "getSampleRate")).intValue();
        int bits = ((Number)get(c, "getBitsPerSample")).intValue();
        j.put("sample_rates_hz", numericFlags(c, "SAMPLE_RATE_", rate));
        j.put("bit_depths", numericFlags(c, "BITS_PER_SAMPLE_", bits));
        if (!le) {
            long mode = ((Number)get(c, "getCodecSpecific1")).longValue();
            j.put("mode", CodecPresentation.mode(profile, type, mode));
        } else {
            int duration = ((Number)get(c, "getFrameDuration")).intValue();
            j.put("frame_durations_us", numericFlags(c, "FRAME_DURATION_", duration));
            j.put("octets_per_frame", get(c, "getOctetsPerFrame"));
        }
        return j;
    }
    static JSONObject route() throws Exception {
        JSONObject j = new JSONObject();
        Class<?> audio = Class.forName("android.media.AudioSystem");
        int devices =
            ((Number)audio.getMethod("getDevicesForStream", int.class).invoke(null, AudioManager.STREAM_MUSIC)).intValue();
        JSONArray names = new JSONArray();
        int bluetoothMask = 0, leMask = 0;
        for (Field f : audio.getFields()) {
            String n = f.getName();
            if (!Modifier.isStatic(f.getModifiers()) || f.getType() != int.class ||
                !n.startsWith("DEVICE_OUT_"))
                continue;
            if (!(n.startsWith("DEVICE_OUT_BLUETOOTH_A2DP") || n.startsWith("DEVICE_OUT_BLE_") ||
                  n.equals("DEVICE_OUT_SPEAKER") || n.equals("DEVICE_OUT_WIRED_HEADSET") ||
                  n.equals("DEVICE_OUT_WIRED_HEADPHONE")))
                continue;
            int value = f.getInt(null);
            if (value != 0 && (devices & value) == value) {
                names.put(n.substring("DEVICE_OUT_".length()));
                if (n.startsWith("DEVICE_OUT_BLE_"))
                    leMask |= value;
                if (n.startsWith("DEVICE_OUT_BLUETOOTH_A2DP"))
                    bluetoothMask |= value;
            }
        }
        j.put("devices", names);
        j.put("transport", leMask != 0 ? "LE Audio" : bluetoothMask != 0 ? "A2DP" : "Other");
        j.put("source", "AudioSystem media route");
        AudioManager manager = (AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
        j.put("audio_mode", manager.getMode());
        return j;
    }
    static JSONArray active(int id) throws Exception {
        JSONArray out = new JSONArray();
        BluetoothProfile profile = profiles == null ? null : profiles.get(id);
        if (profile == null)
            return out;
        List<?> devices =
            (List<?>)call(adapter, "getActiveDevices", new Class<?>[] {int.class}, id);
        if (devices == null) return out;
        for (Object one : devices) {
            if (!(one instanceof BluetoothDevice))
                continue;
            BluetoothDevice d = (BluetoothDevice)one;
            JSONObject j = new JSONObject();
            // Device addresses are deliberately omitted from UI and copied snapshots.
            j.put("device", d.getName() == null ? "Bluetooth device" : d.getName());
            j.put("transport", id == BluetoothProfile.LE_AUDIO ? "LE Audio" : "A2DP");
            if (id == BluetoothProfile.A2DP) {
                try {
                    j.put("stream_started", call(profile, "isA2dpPlaying",
                                                 new Class<?>[] {BluetoothDevice.class}, d));
                } catch (Exception e) {
                    error("A2DP stream state", e);
                }
            }
            try {
                Object c;
                if (id == BluetoothProfile.LE_AUDIO) {
                    int group = ((Number)call(profile, "getGroupId",
                                              new Class<?>[] {BluetoothDevice.class}, d))
                                    .intValue();
                    Object status =
                        call(profile, "getCodecStatus", new Class<?>[] {int.class}, group);
                    c = status == null ? null : get(status, "getOutputCodecConfig");
                } else {
                    Object status =
                        call(profile, "getCodecStatus", new Class<?>[] {BluetoothDevice.class}, d);
                    c = status == null ? null : get(status, "getCodecConfig");
                }
                j.put("codec", codec(c, id == BluetoothProfile.LE_AUDIO));
            } catch (Exception e) {
                j.put("codec", new JSONObject().put("available", false));
                error("codec", e);
            }
            out.put(j);
        }
        return out;
    }
    private static JSONObject encoderHistory(JSONObject telemetry, boolean sameSession, long savedAt) throws Exception {
        JSONObject source = telemetry.optJSONObject("fields"), fields = new JSONObject();
        if (source != null && source.length() <= 100) {
            for (String key : new String[] {"encoder_state", "encoder_level", "quality_level",
                    "feedback_quality_level", "feedback_bitrate_level", "desired_encoder_bitrate_raw", "encoder_frame_bitrate_bps", "qhs", "sample_rate_hz", "calculated_ttp_us"}) {
                JSONObject field = source.optJSONObject(key);
                if (field == null || field.isNull("value")) continue;
                Object value = field.opt("value");
                if (!(value instanceof String || value instanceof Number || value instanceof Boolean)) continue;
                if (value instanceof String && ((String)value).length() > 100) continue;
                JSONObject saved = new JSONObject(field.toString());
                saved.put("fresh", false).put("scope", "history");
                fields.put(key, saved);
            }
        }
        long observedAt = savedAt;
        JSONObject state = fields.optJSONObject("encoder_state");
        if (state == null) state = fields.optJSONObject("quality_level");
        double ageAtSave = state == null ? -1 : state.optDouble("age_seconds", -1);
        if (Double.isFinite(ageAtSave) && ageAtSave >= 0 && ageAtSave * 1000 <= savedAt)
            observedAt = savedAt - (long)(ageAtSave * 1000);
        JSONArray transitions = new JSONArray(), sourceTransitions = telemetry.optJSONArray("encoder_transitions");
        if (sourceTransitions != null && sourceTransitions.length() <= 8) for (int i = 0; i < sourceTransitions.length(); i++) {
            JSONObject entry = sourceTransitions.optJSONObject(i);
            if (entry == null || !entry.optString("state").matches("STATE_ENCLEVEL_[0-4]|STATE_ENCLEVEL_5_LOSSLESS")) continue;
            double age = entry.optDouble("age_seconds", -1);
            if (!Double.isFinite(age) || age < 0 || age * 1000 > savedAt) continue;
            String previous = entry.optString("previous", "");
            transitions.put(new JSONObject().put("state", entry.optString("state"))
                .put("previous", previous.matches("STATE_ENCLEVEL_[0-4]|STATE_ENCLEVEL_5_LOSSLESS") ? previous : JSONObject.NULL)
                .put("observed_at_ms", savedAt - (long)(age * 1000)).put("source", "encoder packet summary"));
        }
        return new JSONObject().put("available", fields.length() > 0).put("current", false)
            .put("same_session", sameSession).put("saved_at_ms", savedAt)
            .put("observed_at_ms", observedAt)
            .put("capture_id", telemetry.optString("capture_id", ""))
            .put("fields", fields).put("encoder_transitions", transitions).put("label", "Historical capture; not current playback");
    }
    private static boolean observationSession(JSONObject observation) {
        try {
            String bt = read(moduleRoot.resolve("state/bt.pid").toString());
            String hal = read(moduleRoot.resolve("state/hal.pid").toString());
            return bt.matches("[1-9][0-9]+") && hal.matches("[1-9][0-9]+") &&
                Files.isDirectory(Paths.get("/proc", bt)) && Files.isDirectory(Paths.get("/proc", hal)) &&
                bt.equals(observation.optString("bluetooth_pid")) && hal.equals(observation.optString("hal_pid")) &&
                BtDiagnostics.start(bt).equals(observation.optString("bluetooth_start")) &&
                BtDiagnostics.start(hal).equals(observation.optString("hal_start")) &&
                read(moduleRoot.resolve("state/bt.start").toString()).equals(observation.optString("bluetooth_start")) &&
                read(moduleRoot.resolve("state/hal.start").toString()).equals(observation.optString("hal_start")) &&
                read("/proc/sys/kernel/random/boot_id").equals(observation.optString("boot_id"));
        } catch (Exception expired) { return false; }
    }
    private static JSONObject savedEncoderHistory() {
        try {
            Path path = DATA.resolve("encoder-history.json");
            String text = read(path.toString());
            if (text.isEmpty()) return null;
            JSONObject saved = new JSONObject(text);
            if (saved.optInt("schema") != 2) return null;
            long at = saved.optLong("collected_at_ms", 0);
            if (at <= 0) at = Files.getLastModifiedTime(path).toMillis();
            return encoderHistory(saved, observationSession(saved), at);
        } catch (Exception invalid) { return null; }
    }
    static JSONObject telemetry(JSONObject route, JSONArray devices) throws Exception {
        JSONObject out =
            new JSONObject().put("available", false).put("reason", "Diagnostics are not running");
        if (!CodecPresentation.SAMSUNG_DZIF.equals(deviceProfile())) return out;
        JSONObject savedHistory = savedEncoderHistory();
        if (savedHistory != null) out.put("history", savedHistory);
        Path telemetry = DATA.resolve("telemetry.json");
        if (!Files.exists(telemetry)) telemetry = LEGACY_DATA.resolve("telemetry.json");
        String text = read(telemetry.toString());
        if (text.isEmpty())
            return out;
        JSONObject t = new JSONObject(text);
        if (t.optInt("schema") != 2)
            return out.put("reason", "Unsupported DSP observation format");
        boolean sameSession = observationSession(t);
        long savedAt = t.optLong("collected_at_ms", 0);
        if (savedAt <= 0) savedAt = Files.getLastModifiedTime(telemetry).toMillis();
        JSONObject history = encoderHistory(t, sameSession, savedAt);
        if (savedHistory != null && savedHistory.optBoolean("available") &&
                savedHistory.optLong("observed_at_ms") >= history.optLong("observed_at_ms")) history = savedHistory;
        else if (savedHistory != null && !hasEncoderHistory(history)) history = savedHistory;
        out.put("history", history).put("running", t.optBoolean("running"));
        if (!t.optBoolean("running"))
            return out.put("reason", t.optString("reason", "Capture ended; showing history separately"));
        if (!sameSession)
            return out.put("reason", "DSP observation belongs to a different session");
        /* The checks above bind historical and live views to the exact process
         * generation. History is never promoted to a current observation. */
        JSONObject loader = t.optJSONObject("loader");
        if (loader != null) {
            double loadAge = System.nanoTime() / 1e9 - loader.optDouble("monotonic_s", -1);
            if (Double.isFinite(loadAge) && loadAge >= 0 && loadAge <= DspLogDecoder.FRESH_SECONDS) {
                loader.put("age_seconds", loadAge).put("fresh", true);
                out.put("loader", loader);
                return out.put("reason", loader.optString("message", "Encoder module load failed"));
            }
        }
        if (!t.optBoolean("available"))
            return out.put("reason", t.optString("reason", "No current encoder observation"));
        double age = System.nanoTime() / 1e9 - t.optDouble("monotonic_s", -1);
        if (!Double.isFinite(age) || age < 0 || age > 3)
            return out.put("reason", "Last DSP observation is stale");
        JSONObject fields = t.optJSONObject("fields");
        if (fields == null || fields.length() > 100) return out.put("reason", "Invalid DSP field schema");
        double current = System.nanoTime() / 1e9;
        for (Iterator<String> keys = fields.keys(); keys.hasNext();) {
            String key = keys.next(); JSONObject field = fields.getJSONObject(key);
            double fieldAge = current - field.optDouble("monotonic_s", -1);
            boolean epochField = "sample_rate_hz".equals(key) && "encoder_epoch".equals(field.optString("scope"));
            boolean fresh = Double.isFinite(fieldAge) && fieldAge >= 0 && (epochField || fieldAge <= DspLogDecoder.FRESH_SECONDS);
            field.put("age_seconds", fieldAge).put("fresh", fresh);
            t.remove(key);
            if (fresh && !field.isNull("value")) t.put(key, field.get("value"));
        }
        if (!"A2DP".equals(route.optString("transport")))
            return out.put("reason", "DSP observation does not match the media route");
        boolean match = false;
        int a2dpContexts = 0;
        for (int i = 0; i < devices.length(); i++) {
            JSONObject d = devices.getJSONObject(i), c = d.optJSONObject("codec");
            if (!"A2DP".equals(d.optString("transport")))
                continue;
            a2dpContexts++;
            if (c == null) continue;
            if (c.optInt("type", -1) != 7 && c.optInt("type", -1) != 12)
                continue;
            JSONArray rates = c.optJSONArray("sample_rates_hz");
            if (rates != null && rates.length() == 1 &&
                rates.getInt(0) == t.optInt("sample_rate_hz", -1))
                match = true;
        }
        if (!match || a2dpContexts != 1)
            return out.put("reason", "DSP observation does not match the selected codec");
        t.put("available", true).put("age_seconds", age).put("history", history);
        return t;
    }
    private static boolean hasEncoderHistory(JSONObject history) {
        JSONObject fields = history == null ? null : history.optJSONObject("fields");
        return fields != null && (fields.has("encoder_state") || fields.has("quality_level"));
    }
    static JSONObject selectionSnapshot() throws Exception {
        JSONObject result = new JSONObject().put("collected_at_ms", System.currentTimeMillis())
            .put("bluetooth_enabled", adapter != null && adapter.isEnabled());
        JSONObject media;
        try { media = route(); }
        catch (Exception e) { error("media route", e); media = new JSONObject().put("transport", "Unknown"); }
        JSONArray devices = new JSONArray();
        for (int id : new int[] {BluetoothProfile.LE_AUDIO, BluetoothProfile.A2DP}) {
            try {
                JSONArray items = active(id);
                for (int i = 0; i < items.length(); i++) devices.put(items.get(i));
            } catch (Exception e) { error("active profile " + id, e); }
        }
        try {
            ExtensionConfigStore store = new ExtensionConfigStore(DATA.resolve("config.conf"), LEGACY_DATA.resolve("config.conf"), AndroidConfigReader::read, 0);
            result.put("tuning", new JSONObject(CodecTuning.describe(store.read())));
        } catch (Exception error) {
            result.put("tuning", new JSONObject().put("available", false).put("reason", "Configuration unavailable"));
        }
        return result.put("route", media).put("devices", devices);
    }
    static void finish() {
        if (finished)
            return;
        finished = true;
        JSONObject result = new JSONObject();
        String fallback = null;
        try {
            result = selectionSnapshot().put("schema", 1);
            JSONObject r = result.getJSONObject("route");
            JSONArray devices = result.getJSONArray("devices");
            try {
                result.put("dsp", telemetry(r, devices));
            } catch (Exception e) {
                error("DSP observation", e);
                result.put("dsp", new JSONObject()
                                      .put("available", false)
                                      .put("reason", "DSP observation is unavailable"));
            }
            result.put("lossless_transmission", "Unverified");
            result.put("errors", errors);
        } catch (Exception e) {
            fallback = "{\"schema\":1,\"error\":\"Status snapshot failed\"}";
        } finally {
            if (profiles != null) profiles.close();
            System.out.println(fallback == null ? result.toString() : fallback);
            System.exit(0);
        }
    }
    public static void main(String[] args) {
        try {
            if (args.length > 1) throw new IllegalArgumentException("optional module root only");
            moduleRoot = args.length == 1 ? Paths.get(args[0]) : null;
            BluetoothRuntime runtime = BluetoothRuntime.connect();
            context = runtime.context;
            adapter = runtime.adapter;
            if (adapter == null || !adapter.isEnabled()) {
                finish();
                return;
            }
            profiles = new BluetoothProfiles(runtime,
                new int[] {BluetoothProfile.A2DP, BluetoothProfile.LE_AUDIO},
                (id, operation, exception) -> error("profile " + id + " " + operation, exception));
            profiles.start(2500, BtStatus::finish);
            Looper.loop();
        } catch (Exception e) {
            error("Bluetooth service", e);
            finish();
        }
    }
}
