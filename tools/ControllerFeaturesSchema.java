// SPDX-License-Identifier: GPL-2.0-only
/** Pinned Hamilton FC1D wire schema; capabilities are not negotiated link state. */
final class ControllerFeaturesSchema {
    static final int OCF = 0x1d;
    static byte[] request() { return new byte[0]; }
    static boolean permit(int uid, int ocf, byte[] p) {
        return uid == 0 && ocf == OCF && p != null && p.length == 0;
    }
    static int status(int ocf, byte[] p) {
        if (ocf != OCF || p == null || p.length < 1 || p.length > 255)
            throw new IllegalArgumentException("unexpected completion");
        return p[0] & 255;
    }
    static boolean accepted(int ocf, byte[] p) {
        return status(ocf, p) == 0 && p.length == 10 && p[1] == 25 && p[2] == 0 && p[3] == 1 &&
            p[4] == 0;
    }
    static String hex(byte[] p, int start, int end) {
        StringBuilder b = new StringBuilder();
        for (int i = start; i < end; i++)
            b.append(String.format(java.util.Locale.ROOT, "%02x", p[i] & 255));
        return b.toString();
    }
    static String json(byte[] p) {
        int status = status(OCF, p);
        boolean valid = accepted(OCF, p);
        String fields = valid
                            ? "\"product_id\":25,\"response_version\":1,\"features_hex\":\"" +
                                  hex(p, 5, 10) + "\",\"local_enabled\":" + ((p[8] & 0x30) == 0x30)
                            : "\"product_id\":null,\"response_version\":null,\"features_hex\":" +
                              "null,\"local_enabled\":null";
        return "{\"controller_status\":" + status + ",\"response_hex\":\"" + hex(p, 0, p.length) +
            "\",\"valid\":" + valid + "," + fields + ",\"negotiated_qhs\":\"unverified\"}";
    }
}
