// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

public final class DspObservationSessionTest {
    static int checks;
    static void check(boolean condition) { checks++; if (!condition) throw new AssertionError("check " + checks); }
    public static void main(String[] args) {
        DspObservationSession session = new DspObservationSession();
        check(session.update(false, "hq", 0).start == null);
        String first = session.update(true, "hq", 1).start;
        check(first != null && first.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}"));
        check(session.update(true, "hq", 20000).renew == null); // A queued worker has not created its lease yet.
        check(session.started(first, 1));
        check(session.update(true, "hq", 500).renew == null);
        check(first.equals(session.update(true, "hq", 1001).renew));
        DspObservationSession.Action changed = session.update(true, "ll", 1100);
        check(first.equals(changed.stop));
        check(changed.start != null && !changed.start.equals(first));
        String second = changed.start;
        check(!session.completed(first, true, 1101)); // A late previous worker cannot stop its replacement.
        check(!session.started(first, 1101));
        check(session.owns(second));
        check(second.equals(session.update(false, "ll", 1200).stop)); // Page leave, background, idle or unsupported codec.
        check(!session.owns(second));
        check(session.update(false, "ll", 20000).renew == null);
        String third = session.update(true, "ll", 20001).start;
        check(third != null && !third.equals(second));
        check(session.completed(third, true, 20010));
        check(session.update(true, "ll", 24000).start == null); // A failed backend has bounded retry rate.
        String fourth = session.update(true, "ll", 25010).start;
        check(fourth != null);
        check(session.completed(fourth, false, 26000)); // Renew a bounded 120-second reader with a new token.
        check(session.update(true, "ll", 26249).start == null);
        String fifth = session.update(true, "ll", 26250).start;
        check(fifth != null && !fifth.equals(fourth));
        check(fifth.equals(session.update(false, "ll", 26251).stop));
        check(!session.completed(fifth, false, 26252));
        check(ObservationFreshness.current(2.9, 1000, 1050, 3000));
        check(!ObservationFreshness.current(2.9, 1000, 1101, 3000));
        check(!ObservationFreshness.current(0, 1000, 5000, 3000)); // A delayed config call cannot renew an earlier status receipt.
        check(!ObservationFreshness.current(0, 1000, 999, 3000));
        check(!ObservationFreshness.current(Double.NaN, 1000, 1000, 3000));
        check(!ObservationFreshness.current(Double.POSITIVE_INFINITY, 1000, 1000, 3000));
        check(!ObservationFreshness.current(-1, 1000, 1000, 3000));
        check(!ObservationFreshness.current(3, 1000, 1000, 3000));
        long expiry = ObservationFreshness.expiryDelay(2.9995, 1000, 1000, 3000);
        check(expiry >= 1 && !ObservationFreshness.current(2.9995, 1000, 1000 + expiry, 3000));
        check(ObservationFreshness.expiryDelay(5, 1000, 2000, 3000) == 1);
        System.out.println("Foreground DSP observation: " + checks + " checks passed.");
    }
}
