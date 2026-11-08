// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.content.Context;
import android.media.AudioManager;
import android.os.Handler;
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
    static final Map<Integer, BluetoothProfile> profiles = new HashMap<>();
    static final Set<Integer> pending = new HashSet<>();
    static final JSONArray errors = new JSONArray();
    static boolean finished;
    static Handler handler;
    static Object call(Object target, String name, Class<?>[] types, Object... args)
        throws Exception {
        return target.getClass().getMethod(name, types).invoke(target, args);
    }
    static Object get(Object target, String name) throws Exception {
        return call(target, name, new Class<?>[] {});
    }
    static String read(String path) throws Exception {
        Path p = Paths.get(path);
        if (!Files.isRegularFile(p) || Files.size(p) > 32768)
            return "";
        return new String(Files.readAllBytes(p), StandardCharsets.UTF_8).trim();
    }
    static void error(String scope, Exception e) {
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
    static JSONObject codec(Object c, boolean le) throws Exception {
        JSONObject j = new JSONObject();
        if (c == null)
            return j.put("available", false);
        j.put("available", true);
        int type = ((Number)get(c, "getCodecType")).intValue();
        j.put("type", type);
        String name;
        if (!le && type == 12)
            name = "aptX Lossless (selection)";
        else if (!le && type == 13)
            name = "aptX LL";
        else {
            try {
                name = String.valueOf(get(c, "getCodecName"));
            } catch (NoSuchMethodException e) {
                Matcher match = Pattern.compile("codecName[:=]([^,}\\]]+)").matcher(c.toString());
                name = match.find() ? match.group(1).trim() : "Codec " + type;
            }
        }
        j.put("name", name);
        int rate = ((Number)get(c, "getSampleRate")).intValue();
        int bits = ((Number)get(c, "getBitsPerSample")).intValue();
        j.put("sample_rates_hz", numericFlags(c, "SAMPLE_RATE_", rate));
        j.put("bit_depths", numericFlags(c, "BITS_PER_SAMPLE_", bits));
        if (!le) {
            long mode = ((Number)get(c, "getCodecSpecific1")).longValue();
            j.put("mode", type == 12     ? "Lossless request"
                          : mode == 4096 ? "HQ"
                          : mode == 8192 ? "Low latency"
                                         : "Default");
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
            ((Number)audio.getMethod("getDevicesForStream", int.class).invoke(null, 3)).intValue();
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
        BluetoothProfile profile = profiles.get(id);
        if (profile == null)
            return out;
        List<?> devices =
            (List<?>)call(adapter, "getActiveDevices", new Class<?>[] {int.class}, id);
        for (Object one : devices) {
            if (!(one instanceof BluetoothDevice))
                continue;
            BluetoothDevice d = (BluetoothDevice)one;
            JSONObject j = new JSONObject();
            // Device addresses are deliberately omitted from UI and copied snapshots.
            j.put("device", d.getName() == null ? "Bluetooth device" : d.getName());
            j.put("transport", id == 22 ? "LE Audio" : "A2DP");
            try {
                Object c;
                if (id == 22) {
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
                j.put("codec", codec(c, id == 22));
            } catch (Exception e) {
                j.put("codec", new JSONObject().put("available", false));
                error("codec", e);
            }
            out.put(j);
        }
        return out;
    }
    static JSONObject telemetry(JSONObject route, JSONArray devices) throws Exception {
        JSONObject out =
            new JSONObject().put("available", false).put("reason", "Diagnostics are not running");
        String text = read("/data/adb/s25_bluetooth/telemetry.json");
        if (text.isEmpty())
            return out;
        JSONObject t = new JSONObject(text);
        double age = System.nanoTime() / 1e9 - t.optDouble("monotonic_s", -1);
        if (!Double.isFinite(age) || age < 0 || age > 3)
            return out.put("reason", "Last DSP observation is stale");
        String bt = read("/data/adb/modules/s25_aptx_adaptive/state/bt.pid");
        String hal = read("/data/adb/modules/s25_aptx_adaptive/state/hal.pid");
        if (!bt.matches("[1-9][0-9]+") || !hal.matches("[1-9][0-9]+") ||
            !Files.isDirectory(Paths.get("/proc", bt)) ||
            !Files.isDirectory(Paths.get("/proc", hal)) ||
            !bt.equals(t.optString("bluetooth_pid")) || !hal.equals(t.optString("hal_pid")))
            return out.put("reason", "DSP observation belongs to a different session");
        if (!"A2DP".equals(route.optString("transport")))
            return out.put("reason", "DSP observation does not match the media route");
        boolean match = false;
        for (int i = 0; i < devices.length(); i++) {
            JSONObject d = devices.getJSONObject(i), c = d.optJSONObject("codec");
            if (!"A2DP".equals(d.optString("transport")) || c == null)
                continue;
            if (c.optInt("type", -1) != 7 && c.optInt("type", -1) != 12)
                continue;
            JSONArray rates = c.optJSONArray("sample_rates_hz");
            if (rates != null && rates.length() == 1 &&
                rates.getInt(0) == t.optInt("sample_rate_hz", -1))
                match = true;
        }
        if (!match)
            return out.put("reason", "DSP observation does not match the selected codec");
        t.put("available", true).put("age_seconds", age);
        return t;
    }
    static void finish() {
        if (finished)
            return;
        finished = true;
        JSONObject result = new JSONObject();
        try {
            result.put("schema", 1).put("collected_at_ms", System.currentTimeMillis());
            result.put("bluetooth_enabled", adapter != null && adapter.isEnabled());
            JSONObject r;
            try {
                r = route();
            } catch (Exception e) {
                error("media route", e);
                r = new JSONObject().put("transport", "Unknown").put("source", "Unavailable");
            }
            result.put("route", r);
            JSONArray devices = new JSONArray();
            for (int id : new int[] {22, 2}) {
                try {
                    JSONArray items = active(id);
                    for (int i = 0; i < items.length(); i++)
                        devices.put(items.get(i));
                } catch (Exception e) {
                    error("active profile " + id, e);
                }
            }
            result.put("devices", devices);
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
            System.out.println(result.toString());
        } catch (Exception e) {
            System.out.println("{\"schema\":1,\"error\":\"Status snapshot failed\"}");
        } finally {
            if (adapter != null)
                for (Map.Entry<Integer, BluetoothProfile> p : profiles.entrySet())
                    adapter.closeProfileProxy(p.getKey(), p.getValue());
            System.exit(0);
        }
    }
    public static void main(String[] args) {
        try {
            Looper.prepareMainLooper();
            Class<?> manager = Class.forName("android.os.BluetoothServiceManager");
            Class.forName("android.bluetooth.BluetoothFrameworkInitializer")
                .getMethod("setBluetoothServiceManager", manager)
                .invoke(null, manager.getConstructor().newInstance());
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("systemMain").invoke(null);
            context = ((Context)at.getMethod("getSystemContext").invoke(thread))
                          .createPackageContext("com.android.shell", 0);
            adapter = context.getSystemService(BluetoothManager.class).getAdapter();
            handler = new Handler(Looper.getMainLooper());
            if (adapter == null || !adapter.isEnabled()) {
                finish();
                return;
            }
            pending.add(2);
            pending.add(22);
            handler.postDelayed(BtStatus::finish, 2500);
            BluetoothProfile.ServiceListener listener = new BluetoothProfile.ServiceListener() {
                public void onServiceConnected(int id, BluetoothProfile proxy) {
                    profiles.put(id, proxy);
                    pending.remove(id);
                    if (pending.isEmpty())
                        handler.postDelayed(BtStatus::finish, 50);
                }
                public void onServiceDisconnected(int id) { profiles.remove(id); }
            };
            for (int id : new int[] {2, 22}) {
                if (!adapter.getProfileProxy(context, listener, id))
                    pending.remove(id);
            }
            if (pending.isEmpty())
                finish();
            Looper.loop();
        } catch (Exception e) {
            error("Bluetooth service", e);
            finish();
        }
    }
}
