// SPDX-License-Identifier: GPL-2.0-only
/** Private codec IDs and mode words are meaningful only for their verified backend. */
final class CodecPresentation {
    static final String SAMSUNG_DZIF = "samsung-sm-s931z-dzif";

    static boolean losslessSelection(String profile, boolean le, int type) {
        return !le && SAMSUNG_DZIF.equals(profile) && type == 12;
    }

    static String name(String profile, boolean le, int type, String systemName) {
        if (losslessSelection(profile, le, type)) return "aptX Lossless (selection)";
        if (!le && SAMSUNG_DZIF.equals(profile) && type == 13) return "aptX LL";
        return systemName == null || systemName.trim().isEmpty() ? "Codec " + type : systemName;
    }

    static String mode(String profile, int type, long value) {
        if (!SAMSUNG_DZIF.equals(profile)) return null;
        if (type == 12) return "Lossless request";
        if (type != 7) return null;
        if (value == 4096) return "HQ";
        if (value == 8192) return "Low latency";
        return value == 0 ? "Default" : null;
    }
}
