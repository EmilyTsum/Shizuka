// SPDX-License-Identifier: GPL-2.0-only
final class ControllerFeaturesTest {
    static int checks;
    static void check(boolean value) {
        checks++;
        if (!value)
            throw new AssertionError("check " + checks);
    }
    static void bad(Runnable work) {
        try {
            work.run();
        } catch (IllegalArgumentException expected) {
            return;
        }
        throw new AssertionError();
    }
    public static void main(String[] args) {
        check(ControllerFeaturesSchema.permit(0, 0x1d, new byte[0]));
        for (int uid : new int[] {1, 1000, 2000})
            check(!ControllerFeaturesSchema.permit(uid, 0x1d, new byte[0]));
        for (int ocf = 0; ocf < 1024; ocf++)
            check(ControllerFeaturesSchema.permit(0, ocf, new byte[0]) == (ocf == 0x1d));
        check(!ControllerFeaturesSchema.permit(0, 0x1d, null));
        for (int n = 1; n < 256; n++)
            check(!ControllerFeaturesSchema.permit(0, 0x1d, new byte[n]));
        byte[] good = {0, 25, 0, 1, 0, 28, 125, 127, (byte)252, 49};
        for (int i = 0; i < 5; i++)
            for (int v = 0; v < 256; v++) {
                byte[] q = good.clone();
                q[i] = (byte)v;
                check(ControllerFeaturesSchema.accepted(0x1d, q) == (q[i] == good[i]));
            }
        for (int v = 0; v < 256; v++) {
            byte[] q = good.clone();
            q[8] = (byte)v;
            String json = ControllerFeaturesSchema.json(q);
            check(json.contains("\"local_enabled\":" + ((v & 0x30) == 0x30)));
            check(json.contains("\"negotiated_qhs\":\"unverified\""));
            check(!json.contains("\"negotiated_qhs\":true"));
        }
        for (int n = 1; n < 256; n++)
            if (n != 10)
                check(!ControllerFeaturesSchema.accepted(0x1d, new byte[n]));
        check(ControllerFeaturesSchema.json(good).contains("\"features_hex\":\"1c7d7ffc31\""));
        byte[] disabled = good.clone();
        disabled[8] = (byte)204;
        check(ControllerFeaturesSchema.json(disabled).contains("\"local_enabled\":false"));
        byte[] rejected = good.clone();
        rejected[0] = 12;
        check(ControllerFeaturesSchema.json(rejected).contains("\"local_enabled\":null"));
        bad(() -> ControllerFeaturesSchema.status(0x51, good));
        bad(() -> ControllerFeaturesSchema.status(0x1d, null));
        bad(() -> ControllerFeaturesSchema.status(0x1d, new byte[0]));
        bad(() -> ControllerFeaturesSchema.status(0x1d, new byte[256]));
        System.out.println("Controller features: " + checks +
                           " fixed READ/schema/status/capability-only checks passed");
    }
}
