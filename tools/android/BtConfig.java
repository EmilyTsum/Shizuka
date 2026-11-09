// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.Paths;
import java.util.Arrays;

/** CLI boundary: extension files and Android device settings have separate ownership. */
public final class BtConfig {
    static final String USAGE = "config.sh get|validate|value KEY|set KEY VALUE|migrate|tuning capabilities|get|set KEY VALUE|plan PROFILE CODEC MODE RATE BITS|" + DevicePreferences.USAGE;
    public static void main(String[] args) {
        try { run(args); }
        catch (Exception e) {
            System.err.println("config_error=" + e.getMessage());
            System.exit(1);
        }
    }
    static void run(String[] args) throws Exception {
        if (args.length < 3) throw new IllegalArgumentException("canonical path, legacy path and action required");
        String action = args[2];
        if ("help".equals(action) || "--help".equals(action)) {
            requireCount(args, 3);
            System.out.println(USAGE);
            return;
        }
        // Device operations do not depend on, read, or write the extension configuration.
        if ("device".equals(action)) {
            if (args.length < 4) throw new IllegalArgumentException(DevicePreferences.USAGE);
            String address = DevicePreferences.address(args[3]);
            DevicePreferences.Command command = DevicePreferences.parse(Arrays.copyOfRange(args, 4, args.length));
            SystemBluetoothSettings.run(BluetoothRuntime.connect(), address, command);
            return;
        }
        ExtensionConfigStore store = new ExtensionConfigStore(Paths.get(args[0]), Paths.get(args[1]), AndroidConfigReader::read, 0);
        if ("tuning".equals(action)) {
            if (args.length < 4) throw new IllegalArgumentException("tuning capabilities|get|set KEY VALUE|plan PROFILE CODEC MODE RATE BITS");
            switch (args[3]) {
            case "capabilities": requireCount(args, 4); System.out.println(ControlJson.encode(CodecTuning.capabilities())); return;
            case "get": requireCount(args, 4); System.out.println(ControlJson.encode(CodecTuning.describe(store.read()))); return;
            case "set":
                requireCount(args, 6);
                String key = CodecTuning.key(args[4]), value = CodecTuning.value(key, args[5]);
                store.update(config -> config.with(key, value));
                java.util.Map<String, Object> saved = CodecTuning.describe(store.read());
                saved.put("accepted_change", ControlJson.object("key", key, "value", Integer.parseInt(value)));
                saved.put("saved", true);
                System.out.println(ControlJson.encode(saved)); return;
            case "plan":
                requireCount(args, 9);
                System.out.println(ControlJson.encode(CodecTuning.plan(store.read(), args[4], args[5], args[6], Integer.parseInt(args[7]), Integer.parseInt(args[8])))); return;
            default: throw new IllegalArgumentException("Unknown tuning action");
            }
        }
        if ("set".equals(action)) {
            requireCount(args, 5);
            ExtensionConfig.defaults().with(args[3], args[4]); // Validate before creating lock/state.
            store.update(config -> config.with(args[3], args[4]));
            if (CodecTuning.KEY.equals(args[3]) || CodecTuning.LL_KEY.equals(args[3])) {
                System.out.println(ControlJson.encode(CodecTuning.describe(store.read()))); return;
            }
            System.out.println("extension saved; apply native changes with rollback followed by KernelSU Action");
            return;
        }
        if ("migrate".equals(action)) {
            requireCount(args, 3);
            store.update(ExtensionConfig::forMigration);
            System.out.println("minimal extension configuration saved; Android device preferences unchanged");
            return;
        }
        ExtensionConfig config = store.read();
        switch (action) {
        case "get":
            requireCount(args, 3);
            for (String key : ExtensionConfig.KEYS) System.out.println(key + "=" + config.value(key));
            if (config.legacy) System.out.println("legacy_device_preferences=ignored; use Android Bluetooth settings");
            return;
        case "validate":
            requireCount(args, 3); System.out.println("extensions=valid; device_preferences=android"); return;
        case "value":
            requireCount(args, 4);
            System.out.println("classic_qhs_host_aware".equals(args[3]) ? Boolean.toString(config.qhsEnabled()) : config.value(args[3]));
            return;
        case "watch":
            requireCount(args, 3);
            if (!config.qhsEnabled()) { System.out.println("extension_watch=disabled"); return; }
            new QhsController(BluetoothRuntime.connect(), store).watch(); return;
        default:
            throw new IllegalArgumentException(USAGE + "; codec selection belongs to Android");
        }
    }
    private static void requireCount(String[] args, int count) {
        if (args.length != count) throw new IllegalArgumentException("unexpected arguments");
    }
}
