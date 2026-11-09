// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.os.*;
import java.util.*;

/** Android's Bluetooth service is the sole store for per-device preferences. */
final class SystemBluetoothSettings implements DevicePreferences.SystemStore {
    private final BluetoothRuntime runtime;
    private final BluetoothDevice device;
    private BluetoothProfiles profiles;
    private boolean finished;
    private SystemBluetoothSettings(BluetoothRuntime runtime, BluetoothDevice device) {
        this.runtime = runtime; this.device = device;
    }
    private static Object call(Object target, String name, Class<?>[] types, Object... args)
        throws Exception { return BluetoothRuntime.call(target, name, types, args); }
    private static BluetoothDevice resolve(BluetoothAdapter adapter, String address) throws Exception {
        if ("active".equals(address)) {
            Map<String, BluetoothDevice> active = new LinkedHashMap<>();
            for (int profile : new int[] {BluetoothProfile.A2DP, BluetoothProfile.LE_AUDIO, BluetoothProfile.HEADSET}) {
                List<?> list;
                try { list = (List<?>)call(adapter, "getActiveDevices", new Class<?>[] {int.class}, profile); }
                catch (UnsupportedOperationException | IllegalArgumentException unsupported) { continue; }
                if (list == null) continue;
                for (Object item : list) if (item instanceof BluetoothDevice) {
                    BluetoothDevice device = (BluetoothDevice)item;
                    active.put(device.getAddress(), device);
                }
            }
            if (active.size() != 1)
                throw new IllegalStateException("no unique active device; specify a bonded address");
            return active.values().iterator().next();
        }
        if (!BluetoothAdapter.checkBluetoothAddress(address.toUpperCase(Locale.ROOT)))
            throw new IllegalArgumentException("invalid Bluetooth address");
        for (BluetoothDevice device : adapter.getBondedDevices())
            if (device.getAddress().equalsIgnoreCase(address)) return device;
        throw new IllegalArgumentException("device is not bonded");
    }
    private static int profileId(String name) {
        return "a2dp".equals(name) ? BluetoothProfile.A2DP
            : "headset".equals(name) ? BluetoothProfile.HEADSET : BluetoothProfile.LE_AUDIO;
    }
    private BluetoothProfile profile(int id) throws Exception {
        return profiles.require(id);
    }
    private static String unavailable(Exception error) {
        return "unavailable:" + (error instanceof ProfileSession.Unavailable
            ? ((ProfileSession.Unavailable)error).state.label : error.getClass().getSimpleName());
    }
    private Bundle preferred() throws Exception {
        Bundle bundle = (Bundle)call(runtime.adapter, "getPreferredAudioProfiles", new Class<?>[] {BluetoothDevice.class}, device);
        return bundle == null ? new Bundle() : bundle;
    }
    public Map<String, String> read() throws Exception {
        Map<String, String> values = new LinkedHashMap<>();
        values.put("storage", "android");
        try {
            Bundle saved = preferred();
            for (String mode : new String[] {"output", "duplex"}) {
                int id = saved.getInt("output".equals(mode) ? "audio_mode_output_only" : "audio_mode_duplex", -1);
                values.put("transport_" + mode, id == BluetoothProfile.LE_AUDIO ? "le"
                    : id == BluetoothProfile.A2DP || id == BluetoothProfile.HEADSET ? "classic" : "system");
            }
        } catch (Exception e) {
            for (String mode : new String[] {"output", "duplex"}) values.put("transport_" + mode, "unavailable:" + e.getClass().getSimpleName());
        }
        for (String name : new String[] {"a2dp", "headset", "le"}) {
            try {
                int policy = (Integer)call(profile(profileId(name)), "getConnectionPolicy", new Class<?>[] {BluetoothDevice.class}, device);
                values.put("profile_" + name, policy == 100 ? "true" : policy == 0 ? "false" : "system");
            } catch (Exception e) { values.put("profile_" + name, unavailable(e)); }
        }
        try {
            int value = (Integer)call(profile(BluetoothProfile.A2DP), "isOptionalCodecsEnabled", new Class<?>[] {BluetoothDevice.class}, device);
            values.put("optional_codecs", value == 1 ? "true" : value == 0 ? "false" : "system");
        } catch (Exception e) { values.put("optional_codecs", unavailable(e)); }
        try {
            Object status = call(profile(BluetoothProfile.A2DP), "getCodecStatus", new Class<?>[] {BluetoothDevice.class}, device);
            Object codec = status == null ? null : call(status, "getCodecConfig", new Class<?>[] {});
            values.put("codec", codec == null ? "not_connected" : codec.toString());
        } catch (Exception e) { values.put("codec", unavailable(e)); }
        return values;
    }
    public void transport(String mode, String value) throws Exception {
        String key = "output".equals(mode) ? "audio_mode_output_only" : "audio_mode_duplex";
        int wanted = "le".equals(value) ? BluetoothProfile.LE_AUDIO
            : "output".equals(mode) ? BluetoothProfile.A2DP : BluetoothProfile.HEADSET;
        if (preferred().getInt(key, -1) != wanted) {
            // Submit only the changed key; do not replay another profile's saved value.
            Bundle change = new Bundle(); change.putInt(key, wanted);
            int result = (Integer)call(runtime.adapter, "setPreferredAudioProfiles",
                new Class<?>[] {BluetoothDevice.class, Bundle.class}, device, change);
            if (result != 0) throw new IllegalStateException("Android rejected profile preference: " + result);
        }
        if (preferred().getInt(key, -1) != wanted)
            throw new IllegalStateException("Android has not confirmed the profile preference");
    }
    public void profile(String name, boolean enabled) throws Exception {
        BluetoothProfile profile = profile(profileId(name));
        int wanted = enabled ? 100 : 0;
        int current = (Integer)call(profile, "getConnectionPolicy", new Class<?>[] {BluetoothDevice.class}, device);
        if (current != wanted && !(Boolean)call(profile, "setConnectionPolicy",
                new Class<?>[] {BluetoothDevice.class, int.class}, device, wanted))
            throw new IllegalStateException("Android rejected connection policy");
        if ((Integer)call(profile, "getConnectionPolicy", new Class<?>[] {BluetoothDevice.class}, device) != wanted)
            throw new IllegalStateException("Android has not confirmed connection policy");
    }
    public void optionalCodecs(int preference) throws Exception {
        BluetoothProfile a2dp = profile(BluetoothProfile.A2DP);
        int current = (Integer)call(a2dp, "isOptionalCodecsEnabled", new Class<?>[] {BluetoothDevice.class}, device);
        if (current != preference) call(a2dp, "setOptionalCodecsEnabled", new Class<?>[] {BluetoothDevice.class, int.class}, device, preference);
        if ((Integer)call(a2dp, "isOptionalCodecsEnabled", new Class<?>[] {BluetoothDevice.class}, device) != preference)
            throw new IllegalStateException("Android has not confirmed optional codec preference");
        if (preference != -1 && a2dp.getConnectedDevices().contains(device))
            call(a2dp, preference == 1 ? "enableOptionalCodecs" : "disableOptionalCodecs", new Class<?>[] {BluetoothDevice.class}, device);
    }
    private void finish(DevicePreferences.Command command) {
        if (finished) return;
        finished = true;
        int status = 0;
        try {
            for (Map.Entry<String, String> entry : DevicePreferences.execute(this, command).entrySet())
                System.out.println(entry.getKey() + "=" + entry.getValue());
        } catch (Exception e) { System.err.println("system_settings_error=" + e.getMessage()); status = 1; }
        finally {
            profiles.close();
        }
        System.exit(status);
    }
    static void run(BluetoothRuntime runtime, String address, DevicePreferences.Command command) throws Exception {
        if (!runtime.adapter.isEnabled()) throw new IllegalStateException("Bluetooth is off");
        SystemBluetoothSettings settings = new SystemBluetoothSettings(runtime, resolve(runtime.adapter, address));
        settings.profiles = new BluetoothProfiles(runtime,
            new int[] {BluetoothProfile.A2DP, BluetoothProfile.HEADSET, BluetoothProfile.LE_AUDIO},
            (id, operation, error) -> System.err.println("profile_" + id + "_" + operation + "=" + error.getClass().getSimpleName()));
        settings.profiles.start(3000, () -> settings.finish(command));
        Looper.loop();
    }
}
