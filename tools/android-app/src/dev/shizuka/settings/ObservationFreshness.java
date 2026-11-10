// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

/** Age at API receipt plus all time spent waiting for other work or UI dispatch. */
final class ObservationFreshness {
    private static double remaining(double ageSeconds, long received, long now, long windowMs) {
        if (!Double.isFinite(ageSeconds) || ageSeconds < 0 || received <= 0 || now < received || windowMs <= 0) return -1;
        return windowMs - ageSeconds * 1000 - (now - received);
    }
    static boolean current(double ageSeconds, long received, long now, long windowMs) {
        return remaining(ageSeconds, received, now, windowMs) > 0;
    }
    static long expiryDelay(double ageSeconds, long received, long now, long windowMs) {
        double left = remaining(ageSeconds, received, now, windowMs);
        // Round beyond the deadline so a timer cannot fire just before expiry
        // and leave an unchanged overview labelled current until the next poll.
        return left > 0 ? (long)Math.ceil(left) + 1 : 1;
    }
}
