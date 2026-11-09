// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.*;
import java.util.zip.*;

/** Private, bounded capture artifacts; no network or global Android log export. */
final class DiagnosticStore {
    final Path data, sessions;
    private final int owner;
    DiagnosticStore(Path data, int owner) throws IOException {
        this.data = data; this.owner = owner;
        directory(data); sessions = data.resolve("diagnostics"); directory(sessions);
    }
    void directory(Path path) throws IOException {
        Files.createDirectories(path, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        check(path, true);
    }
    void check(Path path, boolean directory) throws IOException {
        if (!(directory ? Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS) : Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) ||
            ((Number)Files.getAttribute(path, "unix:uid", LinkOption.NOFOLLOW_LINKS)).intValue() != owner ||
            !Files.getPosixFilePermissions(path, LinkOption.NOFOLLOW_LINKS).equals(
                PosixFilePermissions.fromString(directory ? "rwx------" : "rw-------")))
            throw new IOException("Unsafe diagnostic path: " + path.getFileName());
    }
    Path create(Path directory, String name) throws IOException {
        check(directory, true);
        Path out = directory.resolve(name);
        Files.createFile(out, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        return out;
    }
    void atomic(Path directory, String name, Object value) throws IOException {
        check(directory, true);
        Path temp = Files.createTempFile(directory, ".diagnostic-", ".tmp",
            PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            byte[] bytes = (ControlJson.encode(value) + "\n").getBytes(StandardCharsets.UTF_8);
            if (bytes.length > 65536) throw new IOException("Diagnostic snapshot too large");
            Files.write(temp, bytes);
            Files.move(temp, directory.resolve(name), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temp); }
    }
    static String session(String value) {
        if (!value.matches("session-[a-f0-9-]{36}")) throw new IllegalArgumentException("Invalid diagnostic session ID");
        return value;
    }
    Path export(String id) throws IOException {
        Path directory = sessions.resolve(session(id)); check(directory, true);
        check(directory.resolve("summary.json"), false); // Only completed captures may be exported.
        Path output = create(sessions, id + "-" + UUID.randomUUID() + ".zip");
        long total = 0;
        try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(output))) {
            for (String name : Arrays.asList("raw.jsonl", "events.jsonl", "summary.json", "native.stderr")) {
                Path file = directory.resolve(name); check(file, false);
                long size = Files.size(file); total += size;
                if (size > 16 * 1024 * 1024 || total > 32 * 1024 * 1024) throw new IOException("Capture export limit exceeded");
                zip.putNextEntry(new ZipEntry(name)); Files.copy(file, zip); zip.closeEntry();
            }
        } catch (IOException error) { Files.deleteIfExists(output); throw error; }
        return output;
    }
}
