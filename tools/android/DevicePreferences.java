// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** One-shot system operations. This layer has no file store or connect-time defaults. */
final class DevicePreferences {
    interface SystemStore {
        Map<String, String> read() throws Exception;
        void transport(String mode, String value) throws Exception;
        void profile(String profile, boolean enabled) throws Exception;
        void optionalCodecs(int preference) throws Exception;
    }
    static Map<String, String> execute(SystemStore store, String[] args) throws Exception {
        if (args.length == 0 || (args.length == 1 && "get".equals(args[0]))) return store.read();
        if (args.length == 3 && "transport".equals(args[0])) {
            ExtensionConfig.choice("transport mode", args[1], "output", "duplex");
            ExtensionConfig.choice("transport", args[2], "classic", "le");
            store.transport(args[1], args[2]);
        } else if (args.length == 3 && "profile".equals(args[0])) {
            ExtensionConfig.choice("profile", args[1], "a2dp", "headset", "le");
            ExtensionConfig.choice("enabled", args[2], "true", "false");
            store.profile(args[1], "true".equals(args[2]));
        } else if (args.length == 2 && "optional-codecs".equals(args[0])) {
            ExtensionConfig.choice("optional codecs", args[1], "system", "true", "false");
            store.optionalCodecs("system".equals(args[1]) ? -1 : "true".equals(args[1]) ? 1 : 0);
        } else throw new IllegalArgumentException("device ADDRESS [get|transport output|duplex classic|le|profile a2dp|headset|le true|false|optional-codecs system|true|false]");
        return store.read();
    }
}
