// SPDX-License-Identifier: GPL-2.0-only
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.IntConsumer;

/** Exercise discovery order, partial results and resource ownership without Android services. */
public final class ProfileSessionTest {
    static int assertions;
    static void check(boolean value) {
        assertions++;
        if (!value) throw new AssertionError();
    }
    static final class Driver implements ProfileSession.Ops<Object> {
        final ProfileSession<Object> session;
        final List<Integer> requests = new ArrayList<>();
        final List<Object> released = new ArrayList<>();
        final List<String> reports = new ArrayList<>();
        final Map<Integer, Exception> failures = new HashMap<>();
        final Map<Integer, Boolean> accepted = new HashMap<>();
        IntConsumer onRequest;
        Object closeFailure;
        int ready;
        Driver(int... ids) { session = new ProfileSession<>(ids, this); }
        public boolean request(int id) throws Exception {
            requests.add(id);
            if (failures.containsKey(id)) throw failures.get(id);
            if (onRequest != null) onRequest.accept(id);
            return accepted.getOrDefault(id, true);
        }
        public void release(int id, Object proxy) {
            released.add(proxy);
            if (proxy == closeFailure) throw new IllegalStateException("close failed");
        }
        public void report(int id, String operation, Exception error) {
            reports.add(id + ":" + operation + ":" + error.getClass().getSimpleName());
        }
        void start() { session.start(() -> ready++); }
    }
    static void unavailable(ProfileSession<Object> session, int id, ProfileSession.State reason)
        throws Exception {
        try { session.require(id); throw new AssertionError("missing profile was available"); }
        catch (ProfileSession.Unavailable expected) { check(expected.state == reason); }
    }
    static void synchronousCallbacks() throws Exception {
        Driver driver = new Driver(2, 22, 1, 2);
        driver.onRequest = id -> {
            check(driver.ready == 0);
            driver.session.connected(id, new Object());
        };
        driver.session.start(() -> {
            check(driver.requests.size() == 3);
            driver.ready++;
        });
        check(driver.ready == 1);
        check(driver.session.require(2) != null);
        driver.session.expire();
        check(driver.ready == 1);
        driver.session.close();
        driver.session.close();
        check(driver.released.size() == 3);
        unavailable(driver.session, 2, ProfileSession.State.CLOSED);
    }
    static void partialAndUnsupported() throws Exception {
        for (Exception unsupported : new Exception[] {
                null, new UnsupportedOperationException("LE unavailable"), new IllegalArgumentException("profile")}) {
            Driver driver = new Driver(2, 22);
            if (unsupported == null) driver.accepted.put(22, false);
            else driver.failures.put(22, unsupported);
            driver.start();
            check(driver.ready == 0);
            Object a2dp = new Object();
            driver.session.connected(2, a2dp);
            check(driver.ready == 1);
            check(driver.session.require(2) == a2dp);
            unavailable(driver.session, 22, ProfileSession.State.UNSUPPORTED);
            check(driver.reports.get(0).startsWith("22:unsupported:"));
            driver.session.close();
            check(driver.released.size() == 1);
        }
        Driver denied = new Driver(2, 22);
        SecurityException failure = new SecurityException("permission denied");
        denied.failures.put(22, failure);
        denied.start();
        denied.session.connected(2, new Object());
        check(denied.ready == 1);
        try { denied.session.require(22); throw new AssertionError(); }
        catch (SecurityException expected) { check(expected == failure); }
        denied.session.close();
    }
    static void deadlineAndLateCallbacks() throws Exception {
        Driver driver = new Driver(2, 22);
        Object a2dp = new Object(), lateLe = new Object();
        driver.start();
        driver.session.connected(2, a2dp);
        driver.session.expire();
        check(driver.ready == 1);
        check(driver.session.require(2) == a2dp);
        unavailable(driver.session, 22, ProfileSession.State.TIMEOUT);
        driver.session.connected(22, lateLe);
        driver.session.connected(22, lateLe);
        check(driver.released.size() == 1 && driver.released.get(0) == lateLe);
        driver.session.connected(2, a2dp);
        check(driver.released.size() == 1); // A duplicate must not close the usable snapshot.
        check(driver.session.require(2) == a2dp);
        driver.session.disconnected(2);
        check(driver.session.require(2) == a2dp); // Discovery has already completed.
        driver.session.close();
        driver.session.connected(2, a2dp);
        driver.session.expire();
        check(driver.released.size() == 2);
        check(driver.ready == 1);
    }
    static void disconnectAndCleanupFailure() throws Exception {
        Driver driver = new Driver(2, 22);
        Object first = new Object(), replacement = new Object(), le = new Object();
        driver.start();
        driver.session.connected(2, first);
        driver.session.disconnected(2);
        check(driver.session.get(2) == null);
        unavailable(driver.session, 2, ProfileSession.State.DISCONNECTED);
        check(driver.reports.contains("2:disconnected:Unavailable"));
        driver.session.connected(2, replacement);
        driver.session.connected(2, replacement);
        driver.session.connected(22, le);
        check(driver.ready == 1);
        check(driver.session.require(2) == replacement);
        driver.closeFailure = first;
        driver.session.close();
        check(driver.released.size() == 3); // A failed close cannot skip other proxies.
        check(driver.reports.contains("2:close:IllegalStateException"));
        driver.session.close();
        check(driver.released.size() == 3);
    }
    static void cancellationAndInvalidCallbacks() throws Exception {
        Driver driver = new Driver(2, 22);
        driver.onRequest = id -> driver.session.close();
        driver.start();
        check(driver.requests.size() == 1);
        check(driver.ready == 0);
        Object late = new Object();
        driver.session.connected(2, late);
        driver.session.connected(2, late);
        driver.session.expire();
        check(driver.released.size() == 1);
        check(driver.ready == 0);
        Driver unknown = new Driver(2);
        unknown.start();
        Object unwanted = new Object();
        unknown.session.connected(99, unwanted);
        unknown.session.connected(99, unwanted);
        check(unknown.released.size() == 1);
        check(unknown.session.get(99) == null);
        unknown.session.connected(2, null);
        check(unknown.ready == 1);
        try { unknown.session.require(2); throw new AssertionError(); }
        catch (IllegalStateException expected) { check(expected.getMessage().contains("null")); }
        unknown.session.close();
        Driver empty = new Driver();
        empty.start();
        check(empty.ready == 1 && empty.requests.isEmpty());
        empty.session.close();
    }
    public static void main(String[] args) throws Exception {
        synchronousCallbacks();
        partialAndUnsupported();
        deadlineAndLateCallbacks();
        disconnectAndCleanupFailure();
        cancellationAndInvalidCallbacks();
        System.out.println("Profile session: " + assertions + " checks; partial/unsupported services, deadlines, late callbacks, typed errors and complete cleanup passed.");
    }
}
