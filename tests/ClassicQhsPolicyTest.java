// SPDX-License-Identifier: GPL-2.0-only
import java.util.ArrayList;
import java.util.List;
final class ClassicQhsPolicyTest {
    static void check(boolean value) {
        if (!value)
            throw new AssertionError();
    }
    static final class Fake implements ClassicQhsPolicy.Ops {
        int registrations, sends, releases;
        boolean failRegister, failSend;
        long now = 100, token;
        byte[] request;
        List<String> reports = new ArrayList<>();
        public void register(long generation) throws Exception {
            registrations++;
            if (failRegister)
                throw new Exception("register failed");
        }
        public void send(byte[] bytes) throws Exception {
            sends++;
            request = bytes.clone();
            if (failSend)
                throw new Exception("send outcome unknown");
        }
        public void unregister() { releases++; }
        public void armTimeout(long generation) { token = generation; }
        public long nowNanos() { return now++; }
        public void report(String value) { reports.add(value); }
    }
    static void security() {
        check(ClassicQhsPolicy.trustedOptInValue("true", true).equals("true"));
        check(ClassicQhsPolicy.trustedOptInValue("false", true).equals("false"));
        check(ClassicQhsPolicy.trustedOptInValue("true", false).equals("false"));
        check(ClassicQhsPolicy.trustedOptInValue(null, true).equals("false"));
        // A parsed true must not authorize the NVM path when FD metadata fails trust.
        for (int uid : new int[] {0, 1, 1000, 2000})
            for (int mode : new int[] {0100600, 0100620, 0100602, 0100666, 0120600}) {
                boolean trusted = ClassicQhsPolicy.secureConfig(uid, mode);
                check(ClassicQhsPolicy.trustedOptInValue("true", trusted)
                          .equals(uid == 0 && mode == 0100600 ? "true" : "false"));
            }
        for (int mode : new int[] {0100600, 0100640, 0100644, 0100400})
            check(ClassicQhsPolicy.secureConfig(0, mode));
        for (int mode :
             new int[] {0100666, 0100620, 0100602, 0100777, 0040700, 0120700, 0010600, 0140600})
            check(!ClassicQhsPolicy.secureConfig(0, mode));
        for (int uid : new int[] {-1, 1, 1000, 2000})
            check(!ClassicQhsPolicy.secureConfig(uid, 0100600));
        for (int i = 0; i < 3; i++)
            for (int value = 0; value < 256; value++) {
                byte[] p = {1, 0, 3};
                p[i] = (byte)value;
                check(ClassicQhsPolicy.fixedRequest(p) == (value == new int[] {1, 0, 3}[i]));
            }
        check(!ClassicQhsPolicy.fixedRequest(null));
        check(!ClassicQhsPolicy.fixedRequest(new byte[] {1, 0, 3, 0}));
    }
    public static void main(String[] args) {
        security();
        Fake f = new Fake();
        ClassicQhsPolicy p = new ClassicQhsPolicy(f);
        p.update(true, false, true);
        p.update(true, true, false);
        check(f.sends == 0 && f.registrations == 0);
        p.update(true, true, true);
        long first = p.generation;
        check(f.sends == 1 && p.pending && ClassicQhsPolicy.fixedRequest(f.request));
        p.update(true, true, true);
        check(f.sends == 1);
        p.complete(first, 0x51, new byte[] {0, 1});
        check(Boolean.TRUE.equals(p.accepted) && !p.pending && f.releases == 1);
        check(f.reports.get(f.reports.size() - 1).contains("\"negotiated_qhs\":\"unverified\""));
        p.update(true, true, true);
        check(f.sends == 1);
        p.update(false, true, true);
        check(p.accepted == null && p.status == null && !p.on);
        p.update(true, true, true);
        long second = p.generation;
        check(second != first && f.sends == 2);
        p.complete(first, 0x51, new byte[] {0});
        check(p.pending && p.accepted == null);
        p.timeout(first);
        check(p.pending);
        p.complete(second, 0x51, new byte[] {12, 1});
        check(Boolean.FALSE.equals(p.accepted) && p.status == 12 && f.releases == 2);
        p.update(true, true, true);
        check(f.sends == 2); // No busy/rejection retry loop.
        p.serviceLost();
        p.update(true, true, true);
        long third = p.generation;
        check(third != second && f.sends == 3);
        p.timeout(third);
        check(p.accepted == null && !p.pending && f.releases == 3);
        p.complete(third, 0x51, new byte[] {0});
        check(p.accepted == null); // Late success stays expired.
        p.update(true, true, true);
        check(f.sends == 3);

        Fake c = new Fake();
        ClassicQhsPolicy cancelled = new ClassicQhsPolicy(c);
        cancelled.update(true, true, true);
        long token = cancelled.generation;
        cancelled.update(true, false, true);
        check(!cancelled.pending && c.releases == 1 && cancelled.accepted == null);
        cancelled.complete(token, 0x51, new byte[] {0});
        cancelled.update(true, true, true);
        check(c.sends == 1); // Turning opt-in off sends no inverse.
        cancelled.update(false, true, true);
        cancelled.update(true, true, true);
        check(c.sends == 2);

        for (int failure = 0; failure < 2; failure++) {
            Fake broken = new Fake();
            broken.failRegister = failure == 0;
            broken.failSend = failure == 1;
            ClassicQhsPolicy q = new ClassicQhsPolicy(broken);
            q.update(true, true, true);
            check(!q.pending && q.accepted == null && q.phase.equals("request_error"));
            check(broken.sends == (failure == 0 ? 0 : 1) &&
                  broken.releases == (failure == 0 ? 0 : 1));
            q.update(true, true, true);
            check(broken.registrations == 1);
        }
        Fake invalid = new Fake();
        ClassicQhsPolicy q = new ClassicQhsPolicy(invalid);
        q.update(true, true, true);
        q.complete(q.generation, 0x52, new byte[] {0});
        check(q.accepted == null && q.phase.equals("invalid_response") && invalid.releases == 1);
        for (byte[] response : new byte[][] {new byte[] {0}, new byte[] {0, 11},
                                             new byte[] {0, 1, 0}, new byte[] {12, 11}, null}) {
            Fake wrong = new Fake();
            ClassicQhsPolicy rejected = new ClassicQhsPolicy(wrong);
            rejected.update(true, true, true);
            rejected.complete(rejected.generation, 0x51, response);
            check(rejected.accepted == null && rejected.phase.equals("invalid_response") &&
                  wrong.releases == 1);
        }
        Fake commandStatus = new Fake();
        ClassicQhsPolicy rejected = new ClassicQhsPolicy(commandStatus);
        rejected.update(true, true, true);
        rejected.complete(rejected.generation, 0x51, new byte[] {12});
        check(Boolean.FALSE.equals(rejected.accepted) && rejected.status == 12);
        System.out.println(
            "Classic QHS: secure opt-in, exact request, ON epochs, duplicate suppression, "
            + "late callbacks, timeout/rejection/error cleanup and no inverse tests passed");
    }
}
