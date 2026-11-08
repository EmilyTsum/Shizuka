// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.content.*;
import android.os.*;
import java.nio.file.*;
import java.io.*;
import android.system.*;
import java.lang.reflect.*;
import java.util.concurrent.Executor;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** External policy layer. Does not mount binaries, discover UUIDs, or force an active device. */
public class BtConfig {
    static final String[] KEYS = {"schema",
                                  "target_address",
                                  "startup_transport",
                                  "call_transport",
                                  "adaptive_apply_on_connect",
                                  "adaptive_rate",
                                  "adaptive_bits",
                                  "adaptive_mode",
                                  "adaptive_priority",
                                  "aptx_hd_priority",
                                  "aptx_voice_enabled",
                                  "classic_qhs_host_aware",
                                  "media_allowed",
                                  "calls_allowed",
                                  "le_audio_allowed"};
    static final Set<String> ALLOWED = new HashSet<>(Arrays.asList(KEYS));
    static Path file;
    static BluetoothAdapter adapter;
    static Context context;
    static BluetoothProfile a2dp;
    static boolean watching;
    static final Map<Integer, BluetoothProfile> profiles = new HashMap<>();
    static boolean initialApplied;
    static ClassicQhsPolicy qhsPolicy;
    static final class Config extends LinkedHashMap<String, String> {
        final boolean trusted;
        Config(boolean trusted) { this.trusted = trusted; }
    }
    static void say(String s) { System.out.println(s); }
    static Map<String, String> read(Path path) throws Exception {
        // Hold the exact opened regular file. Do not authorize a global controller
        // request using a symlink or a path-only owner check followed by a new open.
        FileDescriptor fd = Os.open(path.toString(),
                                    OsConstants.O_RDONLY | OsConstants.O_CLOEXEC |
                                        OsConstants.O_NOFOLLOW | OsConstants.O_NONBLOCK,
                                    0);
        FileInputStream input = null;
        String text;
        boolean trusted;
        try {
            StructStat before = Os.fstat(fd);
            if ((before.st_mode & OsConstants.S_IFMT) != OsConstants.S_IFREG ||
                before.st_size > 8192)
                throw new IllegalArgumentException("config is not a bounded regular file");
            input = new FileInputStream(fd);
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int n;
            while ((n = input.read(buffer)) != -1) {
                if (bytes.size() + n > 8192)
                    throw new IllegalArgumentException("config too large");
                bytes.write(buffer, 0, n);
            }
            StructStat after = Os.fstat(fd);
            trusted = android.os.Process.myUid() == 0 &&
                      ClassicQhsPolicy.secureConfig(before.st_uid, before.st_mode) &&
                      ClassicQhsPolicy.secureConfig(after.st_uid, after.st_mode) &&
                      before.st_dev == after.st_dev && before.st_ino == after.st_ino;
            text = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            if (input != null)
                input.close();
            else
                Os.close(fd);
        }
        return parse(text, trusted);
    }
    static Map<String, String> parse(String text, boolean trusted) {
        Map<String, String> c = new Config(trusted);
        for (String raw : text.split("\\r\\n|\\r|\\n")) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#"))
                continue;
            int at = line.indexOf('=');
            if (at < 1)
                throw new IllegalArgumentException("expected key=value");
            String key = line.substring(0, at).trim(), value = line.substring(at + 1).trim();
            if (!ALLOWED.contains(key) || c.put(key, value) != null)
                throw new IllegalArgumentException("unknown/duplicate key: " + key);
        }
        // Schema 1 files from 0.3 retain all user values; the new independent HD key has a default.
        c.putIfAbsent("aptx_hd_priority", "4001");
        c.putIfAbsent("aptx_voice_enabled", "false");
        c.putIfAbsent("classic_qhs_host_aware", "false");
        validate(c);
        return c;
    }
    static void choice(Map<String, String> c, String key, String... values) {
        if (!Arrays.asList(values).contains(c.get(key)))
            throw new IllegalArgumentException("invalid " + key);
    }
    static void validate(Map<String, String> c) {
        if (!c.keySet().equals(ALLOWED))
            throw new IllegalArgumentException("all schema keys required");
        choice(c, "schema", "1");
        for (String key : new String[] {"media_allowed", "calls_allowed", "le_audio_allowed"})
            choice(c, key, "remember", "true", "false");
        if (!c.get("target_address").matches("(?i)([0-9a-f]{2}:){5}[0-9a-f]{2}"))
            throw new IllegalArgumentException("invalid target_address");
        choice(c, "startup_transport", "le", "classic", "remember");
        choice(c, "call_transport", "le", "classic", "remember");
        choice(c, "adaptive_apply_on_connect", "true", "false");
        choice(c, "adaptive_rate", "44100", "48000", "96000");
        choice(c, "aptx_voice_enabled", "true", "false");
        choice(c, "classic_qhs_host_aware", "true", "false");
        choice(c, "adaptive_bits",
               "24"); // DSP input contract: PCM24/32; explicit PCM16 corrupts audio.
        choice(c, "adaptive_mode", "hq", "ll", "lossless");
        for (String key : new String[] {"adaptive_priority", "aptx_hd_priority"})
            if (!c.get(key).matches("[0-9]{1,6}"))
                throw new IllegalArgumentException("invalid " + key);
    }
    static void write(Map<String, String> c) throws Exception {
        validate(c);
        StringBuilder out =
            new StringBuilder("# S25 Bluetooth policy, schema 1. Never executed as shell code.\n");
        for (String key : KEYS)
            out.append(key).append('=').append(c.get(key)).append('\n');
        Path temporary = file.resolveSibling(file.getFileName() + ".new");
        Files.write(temporary, out.toString().getBytes(StandardCharsets.UTF_8));
        Files.setPosixFilePermissions(
            temporary, java.nio.file.attribute.PosixFilePermissions.fromString("rw-------"));
        Files.move(temporary, file, StandardCopyOption.ATOMIC_MOVE,
                   StandardCopyOption.REPLACE_EXISTING);
    }
    static Object call(Object o, String method, Class<?>[] types, Object... args) throws Exception {
        return o.getClass().getMethod(method, types).invoke(o, args);
    }
    static BluetoothDevice target(Map<String, String> c) {
        for (BluetoothDevice d : adapter.getBondedDevices())
            if (d.getAddress().equalsIgnoreCase(c.get("target_address")))
                return d;
        throw new IllegalStateException("configured device is not bonded");
    }
    static void setup() throws Exception {
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
    }
    static void qhs(Map<String, String> c) {
        if (qhsPolicy != null)
            qhsPolicy.update(adapter.isEnabled(), "true".equals(c.get("classic_qhs_host_aware")),
                             c instanceof Config && ((Config)c).trusted);
    }
    static void startQhsPolicy(Handler handler) {
        qhsPolicy = new ClassicQhsPolicy(new ClassicQhsPolicy.Ops() {
            Object callback;
            Method unregister;
            public long nowNanos() { return System.nanoTime(); }
            public void report(String value) { say(value); }
            public void register(long token) throws Exception {
                if (android.os.Process.myUid() != 0 || !adapter.isEnabled())
                    throw new IllegalStateException("root and initialized adapter required");
                Class<?> api = Class.forName(
                    "android.bluetooth.BluetoothAdapter$BluetoothHciVendorSpecificCallback");
                callback = Proxy.newProxyInstance(
                    api.getClassLoader(), new Class<?>[] {api}, (proxy, method, args) -> {
                        switch (method.getName()) {
                        case "hashCode":
                            return System.identityHashCode(proxy);
                        case "equals":
                            return proxy == args[0];
                        case "toString":
                            return "ClassicQhsPolicyCallback";
                        case "onCommandComplete":
                            qhsPolicy.complete(token, (Integer)args[0],
                                               args[1] == null ? null : ((byte[])args[1]).clone());
                            break;
                        case "onCommandStatus":
                            if ((Integer)args[1] != 0)
                                qhsPolicy.complete(token, (Integer)args[0],
                                                   new byte[] {((Integer)args[1]).byteValue()});
                            break;
                        default:
                            break;
                        }
                        return null;
                    });
                unregister = BluetoothAdapter.class.getMethod(
                    "unregisterBluetoothHciVendorSpecificCallback", api);
                Executor executor = task -> handler.post(task);
                BluetoothAdapter.class
                    .getMethod("registerBluetoothHciVendorSpecificCallback", Set.class,
                               Executor.class, api)
                    .invoke(adapter, Collections.emptySet(), executor, callback);
            }
            public void send(byte[] request) throws Exception {
                if (android.os.Process.myUid() != 0 || !adapter.isEnabled() ||
                    !ClassicQhsPolicy.fixedRequest(request))
                    throw new IllegalStateException("invalid fixed QHS request");
                BluetoothAdapter.class
                    .getMethod("sendBluetoothHciVendorSpecificCommand", int.class, byte[].class)
                    .invoke(adapter, ClassicQhsPolicy.OCF, request);
            }
            public void unregister() throws Exception {
                Object old = callback;
                callback = null;
                if (old != null)
                    unregister.invoke(adapter, old);
            }
            public void armTimeout(long token) {
                handler.postDelayed(() -> qhsPolicy.timeout(token), 5000);
            }
        });
    }
    static void transport(Map<String, String> c) throws Exception {
        BluetoothDevice d = target(c);
        Bundle before = (Bundle)call(adapter, "getPreferredAudioProfiles",
                                     new Class<?>[] {BluetoothDevice.class}, d);
        Bundle b = new Bundle(before);
        boolean changed = false;
        if (!c.get("startup_transport").equals("remember"))
            b.putInt("audio_mode_output_only", c.get("startup_transport").equals("le") ? 22 : 2);
        if (!c.get("call_transport").equals("remember"))
            b.putInt("audio_mode_duplex", c.get("call_transport").equals("le") ? 22 : 1);
        for (String key : b.keySet())
            if (b.getInt(key, -1) != before.getInt(key, -1))
                changed = true;
        if (changed) {
            int result = (Integer)call(adapter, "setPreferredAudioProfiles",
                                       new Class<?>[] {BluetoothDevice.class, Bundle.class}, d, b);
            say("preferred_profile_result=" + result);
            if (result != 0)
                throw new IllegalStateException("native profile preference rejected: " + result);
        }
        Bundle saved = (Bundle)call(adapter, "getPreferredAudioProfiles",
                                    new Class<?>[] {BluetoothDevice.class}, d);
        say("preferred_output=" + saved.getInt("audio_mode_output_only", -1) +
            " preferred_duplex=" + saved.getInt("audio_mode_duplex", -1));
    }
    static void permissions(Map<String, String> c) throws Exception {
        BluetoothDevice d = target(c);
        String[] keys = {"media_allowed", "calls_allowed", "le_audio_allowed"};
        int[] ids = {2, 1, 22};
        for (int i = 0; i < ids.length; i++) {
            String value = c.get(keys[i]);
            if (value.equals("remember"))
                continue;
            BluetoothProfile profile = profiles.get(ids[i]);
            if (profile == null)
                throw new IllegalStateException("profile not ready: " + ids[i]);
            int wanted = value.equals("true") ? 100 : 0;
            int actual = (Integer)call(profile, "getConnectionPolicy",
                                       new Class<?>[] {BluetoothDevice.class}, d);
            if (actual != wanted) {
                boolean ok =
                    (Boolean)call(profile, "setConnectionPolicy",
                                  new Class<?>[] {BluetoothDevice.class, int.class}, d, wanted);
                if (!ok)
                    throw new IllegalStateException("connection policy rejected: " + keys[i]);
            }
            say(keys[i] + "=" + wanted);
        }
    }
    static void adaptive(Map<String, String> c) throws Exception {
        if (!c.get("adaptive_apply_on_connect").equals("true")) {
            say("adaptive_policy=leave_current");
            return;
        }
        if (a2dp == null) {
            say("adaptive_policy=waiting_for_profile");
            return;
        }
        BluetoothDevice d = target(c);
        // Do not open Classic while LE Audio is selected; only configure an already active A2DP
        // route.
        List<?> activeLe =
            (List<?>)call(adapter, "getActiveDevices", new Class<?>[] {int.class}, 22);
        if (activeLe.contains(d)) {
            say("adaptive_policy=deferred_while_le_active");
            return;
        }
        List<?> active = (List<?>)call(adapter, "getActiveDevices", new Class<?>[] {int.class}, 2);
        if (!active.contains(d)) {
            say("adaptive_policy=deferred_until_a2dp_active");
            return;
        }
        Object status = call(a2dp, "getCodecStatus", new Class<?>[] {BluetoothDevice.class}, d);
        if (status == null)
            throw new IllegalStateException("active A2DP has no codec status");
        Class<?> cc = Class.forName("android.bluetooth.BluetoothCodecConfig"),
                 ct = Class.forName("android.bluetooth.BluetoothCodecType");
        Object selected = call(status, "getCodecConfig", new Class<?>[] {});
        Object type = call(selected, "getExtendedCodecType", new Class<?>[] {});
        String name = (String)call(type, "getCodecName", new Class<?>[] {});
        // Preserve an explicit SBC/AAC/etc selection. This policy supplies Adaptive defaults only.
        if (!"aptX Adaptive".equals(name) && !"aptX Lossless".equals(name)) {
            say("adaptive_policy=preserved_codec:" + name);
            return;
        }
        boolean r22 =
            (((Long)call(selected, "getCodecSpecific3", new Class<?>[] {})) & 0x2000L) != 0;
        boolean wantLossless = c.get("adaptive_mode").equals("lossless") && r22;
        int rate = wantLossless ? 1
                                : (c.get("adaptive_rate").equals("96000")   ? 8
                                   : c.get("adaptive_rate").equals("48000") ? 2
                                                                            : 1);
        long mode = c.get("adaptive_mode").equals("ll") ? 8192 : (wantLossless ? 12288 : 4096);
        int bits = wantLossless ? 1 : 2;
        Object selectable = call(status, "getCodecsSelectableCapabilities", new Class<?>[] {});
        for (Object cap : (List<?>)selectable) {
            Object candidate = call(cap, "getExtendedCodecType", new Class<?>[] {});
            if ((wantLossless ? "aptX Lossless" : "aptX Adaptive")
                    .equals(call(candidate, "getCodecName", new Class<?>[] {}))) {
                type = candidate;
                break;
            }
        }
        String effectiveRate = wantLossless ? "44100" : c.get("adaptive_rate");
        String effectiveMode =
            wantLossless ? "lossless" : (c.get("adaptive_mode").equals("ll") ? "ll" : "hq");
        if ((Integer)call(selected, "getSampleRate", new Class<?>[] {}) == rate &&
            (Integer)call(selected, "getBitsPerSample", new Class<?>[] {}) == bits &&
            (Long)call(selected, "getCodecSpecific1", new Class<?>[] {}) == mode) {
            say("adaptive_policy=already_matches");
            return;
        }
        Class<?> builderClass = Class.forName("android.bluetooth.BluetoothCodecConfig$Builder");
        Object builder = builderClass.getConstructor().newInstance();
        call(builder, "setExtendedCodecType", new Class<?>[] {ct}, type);
        call(builder, "setCodecPriority", new Class<?>[] {int.class}, 1000000);
        call(builder, "setSampleRate", new Class<?>[] {int.class}, rate);
        call(builder, "setBitsPerSample", new Class<?>[] {int.class}, bits);
        call(builder, "setChannelMode", new Class<?>[] {int.class}, 2);
        call(builder, "setCodecSpecific1", new Class<?>[] {long.class}, mode);
        call(a2dp, "setCodecConfigPreference", new Class<?>[] {BluetoothDevice.class, cc}, d,
             call(builder, "build", new Class<?>[] {}));
        say("adaptive_requested=" + effectiveRate + "/" +
            (wantLossless ? "CD16 (DSP PCM24)" : "24") + "/" + effectiveMode);
    }
    static void apply(String action) throws Exception {
        Map<String, String> c = read(file); // validate everything before any API mutation
        if (action.equals("apply") || action.equals("apply-permissions") || action.equals("watch"))
            permissions(c);
        if (action.equals("apply") || action.equals("apply-transport") || action.equals("watch")) {
            try {
                transport(c);
            } catch (Exception e) {
                if (!watching)
                    throw e;
                say("transport_deferred=" + e.getMessage());
            }
        }
        if (action.equals("apply") || action.equals("apply-adaptive") || action.equals("watch"))
            adaptive(c);
    }
    public static void main(String[] args) {
        try {
            run(args);
        } catch (Exception e) {
            System.err.println("config_error=" + e.getMessage());
            System.exit(1);
        }
    }
    static void run(String[] args) throws Exception {
        if (args.length < 2)
            throw new IllegalArgumentException("config path and action required");
        file = Paths.get(args[0]);
        String action = args[1];
        Map<String, String> c = read(file);
        if (action.equals("validate")) {
            say("config=valid");
            return;
        }
        if (action.equals("value")) {
            if (args.length != 3 || !ALLOWED.contains(args[2]))
                throw new IllegalArgumentException("value KEY");
            say(c.get(args[2]));
            return;
        }
        if (action.equals("get")) {
            for (String key : KEYS)
                say(key + "=" + c.get(key));
            return;
        }
        if (action.equals("set")) {
            if (args.length != 4 || !ALLOWED.contains(args[2]))
                throw new IllegalArgumentException("set KEY VALUE");
            c.put(args[2], args[3]);
            write(c);
            say("saved; run apply or the narrower apply-transport/apply-adaptive");
            return;
        }
        if (!Arrays
                 .asList("apply", "apply-transport", "apply-adaptive", "apply-permissions", "watch")
                 .contains(action))
            throw new IllegalArgumentException("unknown action");
        setup();
        target(c);
        if (action.equals("apply-transport")) {
            apply(action);
            return;
        }
        watching = action.equals("watch");
        Handler handler = new Handler(Looper.getMainLooper());
        if (watching) {
            startQhsPolicy(handler);
            // Reload on route changes, without responding to codec updates (avoids a feedback
            // loop).
            IntentFilter events =
                new IntentFilter("android.bluetooth.a2dp.profile.action.ACTIVE_DEVICE_CHANGED");
            events.addAction(BluetoothAdapter.ACTION_STATE_CHANGED);
            events.addAction("android.bluetooth.action.LE_AUDIO_CONNECTION_STATE_CHANGED");
            events.addAction("android.bluetooth.action.LE_AUDIO_ACTIVE_DEVICE_CHANGED");
            context.registerReceiver(new BroadcastReceiver() {
                public void onReceive(Context ctx, Intent intent) {
                    BluetoothDevice d = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                    try {
                        // Expire acceptance even when the config was removed or became invalid.
                        if (BluetoothAdapter.ACTION_STATE_CHANGED.equals(intent.getAction()) &&
                            intent.getIntExtra(BluetoothAdapter.EXTRA_STATE,
                                               BluetoothAdapter.ERROR) !=
                                BluetoothAdapter.STATE_ON) {
                            qhsPolicy.serviceLost();
                            return;
                        }
                        Map<String, String> latest = read(file);
                        qhs(latest);
                        if (BluetoothAdapter.ACTION_STATE_CHANGED.equals(intent.getAction()))
                            return;
                        if ("android.bluetooth.action.LE_AUDIO_ACTIVE_DEVICE_CHANGED".equals(
                                intent.getAction())) {
                            adaptive(latest);
                            return;
                        }
                        if (d == null || !d.equals(target(latest)))
                            return;
                        if ("android.bluetooth.a2dp.profile.action.ACTIVE_DEVICE_CHANGED".equals(
                                intent.getAction()))
                            adaptive(latest);
                        else if (intent.getIntExtra(BluetoothProfile.EXTRA_STATE, -1) ==
                                 BluetoothProfile.STATE_CONNECTED)
                            transport(latest);
                    } catch (Exception e) {
                        say("policy_error=" + e);
                    }
                }
            }, events, Context.RECEIVER_EXPORTED);
            qhs(c); // Receiver is registered before inspecting an already-ON adapter.
            say("policy_watch=ready");
        } else
            handler.postDelayed(() -> {
                say("profile_timeout");
                System.exit(2);
            }, 15000);
        BluetoothProfile.ServiceListener listener = new BluetoothProfile.ServiceListener() {
            public void onServiceConnected(int p, BluetoothProfile proxy) {
                profiles.put(p, proxy);
                if (p == 2) {
                    a2dp = proxy;
                    if (watching)
                        try {
                            qhs(read(file));
                        } catch (Exception e) {
                            say("qhs_policy_error=" + e);
                        }
                }
                if (profiles.size() != 3 || initialApplied)
                    return;
                initialApplied = true;
                try {
                    apply(action);
                    if (!watching)
                        System.exit(0);
                } catch (Exception e) {
                    e.printStackTrace();
                    if (!watching)
                        System.exit(1);
                }
            }
            public void onServiceDisconnected(int p) {
                profiles.remove(p);
                if (p == 2) {
                    a2dp = null;
                    if (qhsPolicy != null)
                        qhsPolicy.serviceLost();
                }
            }
        };
        for (int profile : new int[] {2, 1, 22})
            if (!adapter.getProfileProxy(context, listener, profile))
                throw new IllegalStateException("profile unavailable: " + profile);
        Looper.loop();
    }
}
