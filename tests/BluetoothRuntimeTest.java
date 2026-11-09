// SPDX-License-Identifier: GPL-2.0-only
/** Exercise real reflection dispatch with an SDK classpath, without starting Android services. */
public final class BluetoothRuntimeTest {
    public static final class Api {
        public String echo(String value) { return value; }
        public void unsupportedProfile() { throw new IllegalArgumentException("profile unavailable"); }
        public void denied() { throw new SecurityException("permission denied"); }
    }
    public static void main(String[] args) throws Exception {
        Api api = new Api();
        if (!"current".equals(BluetoothRuntime.call(api, "echo", new Class<?>[] {String.class}, "current"))) throw new AssertionError();
        try { BluetoothRuntime.call(api, "missing", new Class<?>[] {}); throw new AssertionError(); }
        catch (UnsupportedOperationException expected) { if (!expected.getMessage().contains("missing")) throw new AssertionError(); }
        try { BluetoothRuntime.call(api, "unsupportedProfile", new Class<?>[] {}); throw new AssertionError(); }
        catch (IllegalArgumentException expected) { if (!expected.getMessage().equals("profile unavailable")) throw new AssertionError(); }
        try { BluetoothRuntime.call(api, "denied", new Class<?>[] {}); throw new AssertionError(); }
        catch (SecurityException expected) { if (!expected.getMessage().equals("permission denied")) throw new AssertionError(); }
        System.out.println("Bluetooth reflection: missing APIs, unsupported profiles and permission errors preserve their type.");
    }
}
