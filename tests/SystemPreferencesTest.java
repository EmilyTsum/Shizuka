// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

public final class SystemPreferencesTest {
    static int assertions;
    static void check(boolean value) { assertions++; if (!value) throw new AssertionError(); }
    static void invalid(Runnable command) {
        boolean rejected = false;
        try { command.run(); } catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected);
    }
    static final class AndroidStore implements DevicePreferences.SystemStore {
        final Map<String, String> system;
        int writes;
        AndroidStore(Map<String, String> system) { this.system = system; }
        public Map<String, String> read() { return new LinkedHashMap<>(system); }
        public void transport(String mode, String value) { writes++; system.put("transport_" + mode, value); }
        public void profile(String name, boolean enabled) { writes++; system.put("profile_" + name, Boolean.toString(enabled)); }
        public void optionalCodecs(int preference) { writes++; system.put("optional_codecs", Integer.toString(preference)); }
    }
    public static void main(String[] args) throws Exception {
        ExtensionConfig defaults = ExtensionConfig.defaults();
        check(defaults.value("schema").equals("2"));
        check(defaults.value("adaptive_priority").equals("system"));
        check(defaults.value("aptx_voice_enabled").equals("system"));
        check(!defaults.qhsEnabled());
        String legacy = "schema=1\ntarget_address=01:02:03:04:05:06\nstartup_transport=classic\ncall_transport=le\n"
            + "adaptive_apply_on_connect=true\nadaptive_rate=48000\nadaptive_bits=24\nadaptive_mode=hq\n"
            + "adaptive_priority=4001\naptx_hd_priority=4001\naptx_voice_enabled=false\nclassic_qhs_host_aware=true\n"
            + "media_allowed=false\ncalls_allowed=true\nle_audio_allowed=remember\n";
        ExtensionConfig old = ExtensionConfig.parse(legacy, true);
        check(old.legacy);
        check(old.qhsEnabled());
        check(old.value("adaptive_priority").equals("system"));
        check(old.value("aptx_hd_priority").equals("system"));
        check(old.value("aptx_voice_enabled").equals("system"));
        String migrated = old.forMigration().serialize();
        check(!migrated.contains("target_address"));
        check(!migrated.contains("adaptive_rate"));
        check(!migrated.contains("startup_transport"));
        check(!migrated.contains("media_allowed"));
        check(ExtensionConfig.parse(migrated, true).qhsEnabled());
        check(!ExtensionConfig.parse(migrated, false).qhsEnabled());
        ExtensionConfig untrusted = ExtensionConfig.parse(legacy, false);
        check(!ExtensionConfig.parse(untrusted.forMigration().serialize(), true).qhsEnabled());
        check(!ExtensionConfig.parse(untrusted.with("adaptive_priority", "0").serialize(), true).qhsEnabled());
        check(ExtensionConfig.parse(untrusted.with("classic_qhs_host_aware", "true").serialize(), true).qhsEnabled());
        check(old.with("adaptive_priority", "0").value("adaptive_priority").equals("0"));
        check(old.with("aptx_voice_enabled", "true").value("aptx_voice_enabled").equals("true"));
        invalid(() -> defaults.with("target_address", "01:02:03:04:05:06"));
        invalid(() -> defaults.with("adaptive_rate", "96000"));
        invalid(() -> defaults.with("adaptive_priority", "-1"));
        invalid(() -> defaults.with("adaptive_priority", "1000000"));
        invalid(() -> defaults.with("aptx_voice_enabled", "yes"));
        invalid(() -> ExtensionConfig.parse("schema=2\nadaptive_rate=48000", true));
        invalid(() -> ExtensionConfig.parse("schema=2\nschema=2", true));
        invalid(() -> ExtensionConfig.parse("schema=3", true));
        Map<String, String> system = new LinkedHashMap<>();
        system.put("codec", "aptX Lossless/44100/16");
        system.put("transport_output", "le");
        system.put("transport_duplex", "classic");
        AndroidStore first = new AndroidStore(system);
        check(DevicePreferences.execute(first, new String[] {}).get("codec").equals("aptX Lossless/44100/16"));
        check(first.writes == 0);
        DevicePreferences.execute(first, new String[] {"transport", "output", "classic"});
        check(system.get("transport_duplex").equals("classic"));
        check(system.get("codec").equals("aptX Lossless/44100/16"));
        // A separate invocation reads Android's state, without a plugin cache or target address.
        AndroidStore second = new AndroidStore(system);
        check(DevicePreferences.execute(second, new String[] {"get"}).get("transport_output").equals("classic"));
        check(second.writes == 0);
        system.put("codec", "SBC/48000/16");
        check(DevicePreferences.execute(second, new String[] {}).get("codec").equals("SBC/48000/16"));
        check(second.writes == 0);
        DevicePreferences.execute(second, new String[] {"profile", "a2dp", "false"});
        check(system.get("profile_a2dp").equals("false"));
        DevicePreferences.execute(second, new String[] {"optional-codecs", "true"});
        check(system.get("optional_codecs").equals("1"));
        DevicePreferences.execute(second, new String[] {"optional-codecs", "system"});
        check(system.get("optional_codecs").equals("-1"));
        int before = second.writes;
        for (String[] invalid : new String[][] {{"transport", "output", "unknown"}, {"profile", "other", "true"},
                {"profile", "a2dp", "yes"}, {"optional-codecs", "2"}, {"codec", "adaptive"}}) {
            try { DevicePreferences.execute(second, invalid); throw new AssertionError("accepted invalid mutation"); }
            catch (IllegalArgumentException expected) { check(second.writes == before); }
        }
        System.out.println("System preferences: " + assertions + " assertions; Android owns per-device state, legacy replay removed.");
    }
}
