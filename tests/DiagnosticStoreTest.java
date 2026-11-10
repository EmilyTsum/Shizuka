// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.*;
import java.util.zip.ZipFile;

public final class DiagnosticStoreTest {
    static int assertions;
    static void check(boolean value) { assertions++; if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("shizuka-diagnostic-test-");
        int uid = ((Number)Files.getAttribute(root, "unix:uid")).intValue();
        try {
            DiagnosticStore store = new DiagnosticStore(root, uid);
            String token = UUID.randomUUID().toString(), other = UUID.randomUUID().toString();
            check(!ObservationLease.renew(store, token, 1));
            try (ObservationLease lease = ObservationLease.begin(store, token, 100)) {
                check(lease.alive(101));
                check(!ObservationLease.stop(store, other) && lease.alive(102));
                check(ObservationLease.renew(store, token, 200));
                check(lease.alive(ObservationLease.TTL_NS + 199));
                check(!lease.alive(ObservationLease.TTL_NS + 200));
                check(!ObservationLease.renew(store, token, ObservationLease.TTL_NS + 200));
            }
            ObservationLease old = ObservationLease.begin(store, token, 500);
            check(ObservationLease.stop(store, token) && !old.alive(501));
            try (ObservationLease replacement = ObservationLease.begin(store, token, 502)) {
                old.close(); check(replacement.alive(503)); // old owner cannot delete a newer generation
                try { ObservationLease.begin(store, token, 504); throw new AssertionError(); }
                catch (FileAlreadyExistsException expected) { assertions++; }
            }
            for (String invalid : Arrays.asList("../x", "", token.toUpperCase(Locale.ROOT), "session-" + token)) {
                try { ObservationLease.stop(store, invalid); throw new AssertionError(); }
                catch (IllegalArgumentException expected) { assertions++; }
            }
            try (ObservationLease lease = ObservationLease.begin(store, UUID.randomUUID().toString())) {
                java.util.concurrent.atomic.AtomicReference<Throwable> error = new java.util.concurrent.atomic.AtomicReference<>();
                Thread reader = new Thread(() -> {
                    try { for (int i = 0; i < 100; i++) if (!lease.alive()) throw new AssertionError(); }
                    catch (Throwable failure) { error.set(failure); }
                });
                reader.start();
                for (int i = 0; i < 100; i++) check(lease.alive());
                reader.join(); check(error.get() == null);
            }
            store.atomic(root, "telemetry.json", ControlJson.object("available", false, "reason", "capture ended"));
            store.check(root.resolve("telemetry.json"), false); assertions++;
            Path session = store.sessions.resolve("session-" + UUID.randomUUID()); store.directory(session);
            for (String name : Arrays.asList("raw.jsonl", "events.jsonl", "native.stderr")) store.create(session, name);
            try { store.export(session.getFileName().toString()); throw new AssertionError(); }
            catch (java.io.IOException expected) { assertions++; }
            store.atomic(session, "summary.json", ControlJson.object("complete", true));
            Path zip = store.export(session.getFileName().toString());
            try (ZipFile archive = new ZipFile(zip.toFile())) { check(archive.size() == 4); }
            for (String id : Arrays.asList("../escape", "session-../../escape", "/absolute")) {
                try { store.export(id); throw new AssertionError(); }
                catch (IllegalArgumentException expected) { assertions++; }
            }
            Files.delete(session.resolve("raw.jsonl"));
            Files.createSymbolicLink(session.resolve("raw.jsonl"), root.resolve("telemetry.json"));
            try { store.export(session.getFileName().toString()); throw new AssertionError(); }
            catch (java.io.IOException expected) { assertions++; }
            Files.setPosixFilePermissions(store.sessions, PosixFilePermissions.fromString("rwxr-xr-x"));
            try { store.atomic(store.sessions, "bad.json", ControlJson.object()); throw new AssertionError(); }
            catch (java.io.IOException expected) { assertions++; }
            Files.setPosixFilePermissions(store.sessions, PosixFilePermissions.fromString("rwx------"));
            try { new DiagnosticStore(root, uid + 1); throw new AssertionError(); }
            catch (java.io.IOException expected) { assertions++; }
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                paths.sorted(Comparator.reverseOrder()).forEach(path -> { try { Files.delete(path); } catch (Exception e) { throw new RuntimeException(e); } });
            }
        }
        System.out.println("Diagnostic storage: " + assertions + " checks; private/atomic snapshots, completed-only export, traversal/symlink/owner rejection passed.");
    }
}
