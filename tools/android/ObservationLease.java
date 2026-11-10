// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.UUID;
import java.util.function.LongSupplier;

/** Foreground observation lifetime. No PID lookup or signal operation exists here. */
final class ObservationLease implements AutoCloseable {
    static final long TTL_NS = 5_000_000_000L;
    private final DiagnosticStore store;
    private final String token, generation;
    private ObservationLease(DiagnosticStore store, String token, String generation) {
        this.store = store; this.token = token; this.generation = generation;
    }
    private static String token(String token) {
        if (token == null || !token.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}"))
            throw new IllegalArgumentException("Invalid observation token");
        return token;
    }
    private static Path path(DiagnosticStore store, String token) {
        return store.data.resolve(".observe-" + token(token) + ".lease");
    }
    private interface Operation<T> { T run() throws IOException; }
    private static synchronized <T> T locked(DiagnosticStore store, Operation<T> operation) throws IOException {
        Path lock = store.data.resolve(".observe-control.lock");
        try { store.create(store.data, lock.getFileName().toString()); } catch (FileAlreadyExistsException ignored) {}
        store.check(lock, false);
        try (FileChannel channel = FileChannel.open(lock, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
             FileLock held = channel.lock()) { return operation.run(); }
    }
    private static String[] read(DiagnosticStore store, Path path) throws IOException {
        if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return null;
        store.check(path, false);
        if (Files.size(path) > 160) throw new IOException("Invalid observation lease");
        String[] fields = new String(Files.readAllBytes(path), StandardCharsets.US_ASCII).split("\\n", -1);
        if (fields.length != 3 || !fields[0].matches("[a-f0-9-]{36}") || !fields[1].matches("[0-9]+") || !fields[2].isEmpty())
            throw new IOException("Invalid observation lease");
        return fields;
    }
    private static boolean fresh(String[] fields, long now) {
        if (fields == null) return false;
        try { long left = Long.parseLong(fields[1]) - now; return left > 0 && left <= TTL_NS; }
        catch (NumberFormatException invalid) { return false; }
    }
    private static void write(DiagnosticStore store, Path path, String generation, long now) throws IOException {
        if (now < 0 || now > Long.MAX_VALUE - TTL_NS) throw new IOException("Invalid observation clock");
        Path temp = Files.createTempFile(store.data, ".observe-update-", ".tmp",
            PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            Files.write(temp, (generation + "\n" + (now + TTL_NS) + "\n").getBytes(StandardCharsets.US_ASCII));
            Files.move(temp, path, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temp); }
    }
    static ObservationLease begin(DiagnosticStore store, String value, long now) throws IOException {
        return begin(store, value, () -> now);
    }
    static ObservationLease begin(DiagnosticStore store, String value) throws IOException {
        return begin(store, value, System::nanoTime);
    }
    private static ObservationLease begin(DiagnosticStore store, String value, LongSupplier clock) throws IOException {
        String token = token(value), generation = UUID.randomUUID().toString();
        return locked(store, () -> {
            Path file = store.create(store.data, path(store, token).getFileName().toString());
            try { write(store, file, generation, clock.getAsLong()); }
            catch (IOException error) { Files.deleteIfExists(file); throw error; }
            return new ObservationLease(store, token, generation);
        });
    }
    static boolean renew(DiagnosticStore store, String value, long now) throws IOException {
        return renew(store, value, () -> now);
    }
    static boolean renew(DiagnosticStore store, String value) throws IOException {
        return renew(store, value, System::nanoTime);
    }
    private static boolean renew(DiagnosticStore store, String value, LongSupplier clock) throws IOException {
        Path file = path(store, value);
        return locked(store, () -> {
            String[] fields = read(store, file);
            long now = clock.getAsLong();
            if (!fresh(fields, now)) return false;
            write(store, file, fields[0], now); return true;
        });
    }
    static boolean stop(DiagnosticStore store, String value) throws IOException {
        Path file = path(store, value);
        return locked(store, () -> {
            if (read(store, file) == null) return false;
            return Files.deleteIfExists(file);
        });
    }
    boolean alive(long now) throws IOException {
        return alive(() -> now);
    }
    boolean alive() throws IOException { return alive(System::nanoTime); }
    private boolean alive(LongSupplier clock) throws IOException {
        return locked(store, () -> {
            String[] fields = read(store, path(store, token));
            return fresh(fields, clock.getAsLong()) && generation.equals(fields[0]);
        });
    }
    public void close() throws IOException {
        locked(store, () -> {
            Path file = path(store, token); String[] fields = read(store, file);
            if (fields != null && generation.equals(fields[0])) Files.deleteIfExists(file);
            return null;
        });
    }
}
