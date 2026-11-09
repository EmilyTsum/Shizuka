// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** One-shot system operations. This layer has no file store or connect-time defaults. */
final class DevicePreferences {
    static final String USAGE = "device ADDRESS|active [get|transport output|duplex classic|le|profile a2dp|headset|le true|false|optional-codecs system|true|false]";
    interface SystemStore {
        Map<String, String> read() throws Exception;
        void transport(String mode, String value) throws Exception;
        void profile(String profile, boolean enabled) throws Exception;
        void optionalCodecs(int preference) throws Exception;
    }
    interface Command {
        void apply(SystemStore store) throws Exception;
    }
    static String address(String value) {
        if ("active".equals(value)) return value;
        if (value == null || !value.matches("(?i)([0-9a-f]{2}:){5}[0-9a-f]{2}"))
            throw new IllegalArgumentException("invalid Bluetooth address; specify a bonded address or active");
        return value.toUpperCase(Locale.ROOT);
    }
    // Parse without contacting Android. Commands capture values, never the caller's mutable array.
    static Command parse(String[] args) {
        if (args.length == 0 || (args.length == 1 && "get".equals(args[0]))) return store -> {};
        if (args.length == 3 && "transport".equals(args[0])) {
            ExtensionConfig.choice("transport mode", args[1], "output", "duplex");
            ExtensionConfig.choice("transport", args[2], "classic", "le");
            String mode = args[1], value = args[2];
            return store -> store.transport(mode, value);
        } else if (args.length == 3 && "profile".equals(args[0])) {
            ExtensionConfig.choice("profile", args[1], "a2dp", "headset", "le");
            ExtensionConfig.choice("enabled", args[2], "true", "false");
            String profile = args[1];
            boolean enabled = "true".equals(args[2]);
            return store -> store.profile(profile, enabled);
        } else if (args.length == 2 && "optional-codecs".equals(args[0])) {
            ExtensionConfig.choice("optional codecs", args[1], "system", "true", "false");
            int preference = "system".equals(args[1]) ? -1 : "true".equals(args[1]) ? 1 : 0;
            return store -> store.optionalCodecs(preference);
        }
        throw new IllegalArgumentException(USAGE);
    }
    static Map<String, String> execute(SystemStore store, String[] args) throws Exception {
        return execute(store, parse(args));
    }
    static Map<String, String> execute(SystemStore store, Command command) throws Exception {
        command.apply(store);
        return store.read();
    }
}
