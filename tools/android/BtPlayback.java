// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.os.Looper;
import java.util.*;

/** One-shot Android A2DP adapter; portable selection logic lives in PlaybackSelection. */
public final class BtPlayback {
    private final BluetoothRuntime runtime;
    private BluetoothProfiles profiles;
    private boolean finished, submitted;
    private BtPlayback(BluetoothRuntime runtime) { this.runtime = runtime; }

    private List<BluetoothDevice> active() throws Exception {
        Object value = BluetoothRuntime.call(runtime.adapter, "getActiveDevices", new Class<?>[] {int.class}, BluetoothProfile.A2DP);
        List<BluetoothDevice> out = new ArrayList<>();
        if (value instanceof Iterable) for (Object item : (Iterable<?>)value)
            if (item instanceof BluetoothDevice && !out.contains(item)) out.add((BluetoothDevice)item);
        return out;
    }
    private BluetoothDevice resolve(BluetoothProfile a2dp, String address) throws Exception {
        List<BluetoothDevice> connected = a2dp.getConnectedDevices();
        if ("active".equals(address)) {
            List<BluetoothDevice> active = active();
            if (active.size() != 1 || !connected.contains(active.get(0)))
                throw new IllegalStateException("No unique connected active A2DP device");
            return active.get(0);
        }
        for (BluetoothDevice device : connected) if (device.getAddress().equalsIgnoreCase(address)) return device;
        throw new IllegalStateException("Device is not connected over A2DP");
    }
    private Map<String, Object> execute(PlaybackSelection.Request request) throws Exception {
        BluetoothProfile a2dp = profiles.require(BluetoothProfile.A2DP);
        BluetoothDevice device = resolve(a2dp, request.address);
        Object status = BluetoothRuntime.call(a2dp, "getCodecStatus", new Class<?>[] {BluetoothDevice.class}, device);
        if (status == null) throw new IllegalStateException("Connected device has no codec status");
        List<Object> capabilities = PlaybackSelection.selectable(status);
        Object selected = null;
        if (request.family != null) {
            selected = PlaybackSelection.build(PlaybackSelection.choose(capabilities, request), request);
            // Recheck connection immediately before the one and only preference write.
            if (!a2dp.getConnectedDevices().contains(device)) throw new IllegalStateException("Device disconnected before selection");
            Object accepted = BluetoothRuntime.call(a2dp, "setCodecConfigPreference",
                new Class<?>[] {BluetoothDevice.class, selected.getClass()}, device, selected);
            if (Boolean.FALSE.equals(accepted)) throw new IllegalStateException("Android rejected codec preference");
            submitted = true;
            status = BluetoothRuntime.call(a2dp, "getCodecStatus", new Class<?>[] {BluetoothDevice.class}, device);
        }
        Object observed = status == null ? null : PlaybackSelection.get(status, "getCodecConfig");
        List<Object> available = new ArrayList<>();
        for (Object codec : capabilities) available.add(PlaybackSelection.describe(codec, true));
        return ControlJson.object("schema", 1, "collected_at_ms", System.currentTimeMillis(),
            "device", ControlJson.object("address", device.getAddress(), "name", device.getName(),
                "connected", a2dp.getConnectedDevices().contains(device), "active", active().contains(device)),
            "observed", PlaybackSelection.describe(observed, false), "selectable", available,
            "requested", request.family == null ? null : request.describe(), "submitted", submitted,
            "applied", false, "observation_matches_request", submitted && PlaybackSelection.matches(observed, selected, request),
            "scope", "Android codec selection snapshot; DSP state and receiver playback are unverified");
    }
    private void finish(PlaybackSelection.Request request) {
        if (finished) return;
        finished = true;
        int exit = 0;
        try { System.out.println(ControlJson.encode(execute(request))); }
        catch (Exception error) { printError(error, submitted); exit = 1; }
        finally { profiles.close(); }
        System.exit(exit);
    }
    private static void printError(Exception error, boolean submitted) {
        System.out.println(ControlJson.encode(ControlJson.object("schema", 1,
            "error", error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage(),
            "submitted", submitted, "applied", false)));
    }
    public static void main(String[] args) {
        try {
            PlaybackSelection.Request request = PlaybackSelection.parse(args);
            BluetoothRuntime runtime = BluetoothRuntime.connect();
            if (!runtime.adapter.isEnabled()) throw new IllegalStateException("Bluetooth is off");
            BtPlayback tool = new BtPlayback(runtime);
            tool.profiles = new BluetoothProfiles(runtime, new int[] {BluetoothProfile.A2DP},
                (id, operation, error) -> System.err.println("playback_profile_" + operation + "=" + error.getClass().getSimpleName()));
            tool.profiles.start(3000, () -> tool.finish(request));
            Looper.loop();
        } catch (Exception error) { printError(error, false); System.exit(1); }
    }
}
