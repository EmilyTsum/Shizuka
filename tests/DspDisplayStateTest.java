// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.util.Arrays;
import java.util.Collections;

public final class DspDisplayStateTest {
    static int checks;
    static void check(boolean ok) { checks++; if (!ok) throw new AssertionError("DSP display check " + checks); }
    static String key(Object value, double age, long receipt, long now) {
        return DspDisplayState.bitrateKey(value, true, age, receipt, now, true);
    }
    /** An unchanged receipt models a status request blocked across all deadlines. */
    static final class Screen {
        final long receipt = 1000;
        long now = receipt, nextAt;
        int rebuilds;
        String displayed;
        void render() {
            long delay = DspDisplayState.nextExpiryDelay(0, Arrays.asList(2.9, 1.5, 0.1), receipt, now);
            nextAt = delay > 0 ? now + delay : 0;
            String visible = ObservationFreshness.current(1.5, receipt, now, 3000) + ":" +
                key(512000L, 0.1, receipt, now);
            if (visible.equals(displayed)) return;
            displayed = visible; rebuilds++;
        }
        void fire() {
            check(nextAt > now);
            now = nextAt; nextAt = 0; render();
        }
    }
    static void chainedExpiryWithoutStatusReply() {
        Screen screen = new Screen(); screen.render();
        check(screen.nextAt == 1101 && screen.rebuilds == 1);
        screen.fire(); // Hidden field expires: the visible key is unchanged.
        check(screen.now == 1101 && screen.rebuilds == 1);
        check(screen.nextAt == 2501); // Must survive that unchanged-render return.
        screen.fire();
        check(screen.now == 2501 && screen.rebuilds == 2);
        check(screen.displayed.equals("false:512000") && screen.nextAt == 3901);
        screen.fire();
        check(screen.now == 3901 && screen.displayed.equals("false:"));
        check(screen.nextAt == 4001);
        screen.fire();
        check(screen.nextAt == 0); // No expired-field 1ms rearm loop.
        check(screen.receipt == 1000); // No fresh API response made the chain progress.
    }
    public static void main(String[] args) {
        for (String page : new String[] {"dsp", "dsp_details", "tuning"}) check(DspDisplayState.pollInterval(page) == 1000);
        for (String page : new String[] {"home", "playback", "module", "diagnostics"}) check(DspDisplayState.pollInterval(page) == 4000);
        String initial = key(349867L, 0.1, 1000, 1000);
        check(initial.equals("349867"));
        check(!initial.equals(key(512000L, 0.1, 2000, 2000))); // Bitrate alone invalidates the overview key.
        check(initial.equals(key(349867L, 0.2, 2000, 2000))); // A newer equal measurement does not rebuild.
        check(initial.equals(key(349867L, 0.1, 1000, 3899)));
        check(key(349867L, 0.1, 1000, 3900).isEmpty()); // 3-second expiry, independent of the next API poll.
        check(key(349867L, 2.9, 1000, 1100).isEmpty()); // Includes queued UI/other-call time.
        check(key(349867L, 0, 0, 1000).isEmpty());
        check(key(349867L, 0, 1000, 999).isEmpty());
        check(key(349867L, -1, 1000, 1000).isEmpty());
        check(key(349867L, Double.NaN, 1000, 1000).isEmpty());
        check(key(349867L, Double.POSITIVE_INFINITY, 1000, 1000).isEmpty());
        for (Object invalid : new Object[] {null, "512000", -1L, 0L, Double.NaN, Double.POSITIVE_INFINITY})
            check(key(invalid, 0, 1000, 1000).isEmpty());
        check(DspDisplayState.bitrateKey(512000L, false, 0, 1000, 1000, true).isEmpty());
        check(DspDisplayState.bitrateKey(512000L, true, 0, 1000, 1000, false).isEmpty());
        check(ObservationFreshness.expiryDelay(0.1, 1000, 1000, 3000) == 2901);
        chainedExpiryWithoutStatusReply();
        check(DspDisplayState.nextExpiryDelay(0.1, Arrays.asList(3.1, -1.0, Double.NaN, null), 1000, 1000) == 2901);
        check(DspDisplayState.nextExpiryDelay(0, Arrays.asList(2.9, 0.1), 1000, 2500) == 1401);
        check(DspDisplayState.nextExpiryDelay(0, Arrays.asList(2.9, 0.1), 2000, 2500) == 2401); // A newer receipt replaces the deadline.
        check(DspDisplayState.nextExpiryDelay(3, Collections.emptyList(), 1000, 1000) == 0);
        check(DspDisplayState.nextExpiryDelay(Double.NaN, Arrays.asList(0.0), 1000, 1000) == 0);
        check(DspDisplayState.nextExpiryDelay(0, Collections.emptyList(), 1000, 4000) == 0);
        check(DspDisplayState.nextExpiryDelay(0, Collections.emptyList(), 0, 1000) == 0);
        check(DspDisplayState.nextExpiryDelay(0, Collections.emptyList(), 1000, 999) == 0);
        System.out.println("DSP display cadence and bitrate: " + checks + " checks passed.");
    }
}
