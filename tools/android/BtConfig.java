// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.Paths;
import java.util.Arrays;

/** CLI boundary: extension files and Android device settings have separate ownership. */
public final class BtConfig {
    public static void main(String[] args) {
        try { run(args); }
        catch (Exception e) {
            System.err.println("config_error=" + e.getMessage());
            System.exit(1);
        }
    }
    static void run(String[] args) throws Exception {
        if (args.length < 3) throw new IllegalArgumentException("canonical path, legacy path and action required");
        ExtensionConfigStore store = new ExtensionConfigStore(Paths.get(args[0]), Paths.get(args[1]));
        String action = args[2];
        // Device operations do not depend on, read, or write the extension configuration.
        if ("device".equals(action)) {
            if (args.length < 4) throw new IllegalArgumentException("device ADDRESS|active [operation]");
            SystemBluetoothSettings.run(BluetoothRuntime.connect(), args[3], Arrays.copyOfRange(args, 4, args.length));
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
        case "set":
            requireCount(args, 5); store.write(config.with(args[3], args[4]));
            System.out.println("extension saved; reactivate to change runtime properties or QHS"); return;
        case "migrate":
            requireCount(args, 3); store.write(config.forMigration());
            System.out.println("minimal extension configuration saved; Android device preferences unchanged"); return;
        case "watch":
            requireCount(args, 3);
            if (!config.qhsEnabled()) { System.out.println("extension_watch=disabled"); return; }
            new QhsController(BluetoothRuntime.connect(), store).watch(); return;
        default:
            throw new IllegalArgumentException("use get|validate|value KEY|set KEY VALUE|migrate|device ADDRESS|active [operation]|watch; codec selection belongs to Android");
        }
    }
    private static void requireCount(String[] args, int count) {
        if (args.length != count) throw new IllegalArgumentException("unexpected arguments");
    }
}
