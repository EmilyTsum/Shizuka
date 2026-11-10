// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

/** Foreground display cadence and the independent frame-estimate display value. */
final class DspDisplayState {
    static long pollInterval(String page) {
        return "dsp".equals(page) || "dsp_details".equals(page) || "tuning".equals(page) ? 1000 : 4000;
    }
    static String bitrateKey(Object value, boolean fieldFresh, double ageSeconds,
            long received, long now, boolean dspCurrent) {
        if (!dspCurrent || !fieldFresh || !(value instanceof Number) ||
            !ObservationFreshness.current(ageSeconds, received, now, 3000)) return "";
        double bitrate = ((Number)value).doubleValue();
        return Double.isFinite(bitrate) && bitrate > 0 ? value.toString() : "";
    }
    static long nextExpiryDelay(double dspAgeSeconds, Iterable<Double> fieldAges,
            long received, long now) {
        if (!ObservationFreshness.current(dspAgeSeconds, received, now, 3000)) return 0;
        long delay = ObservationFreshness.expiryDelay(dspAgeSeconds, received, now, 3000);
        for (Double age : fieldAges) {
            // API freshness is fixed at receipt. Ignore fields whose local deadline
            // has already passed, then arm the next deadline even without a reply.
            if (age != null && ObservationFreshness.current(age, received, now, 3000))
                delay = Math.min(delay, ObservationFreshness.expiryDelay(age, received, now, 3000));
        }
        return delay;
    }
}
