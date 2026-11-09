// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Set;
import java.util.function.UnaryOperator;

/** Serialize read/modify/replace across threads and CLI processes; reads create no files. */
final class ExtensionConfigStore {
    interface Reader {
        ExtensionConfig read(Path path) throws Exception;
    }
    private static final Set<PosixFilePermission> DIRECTORY_MODE = PosixFilePermissions.fromString("rwx------");
    private static final Set<PosixFilePermission> FILE_MODE = PosixFilePermissions.fromString("rw-------");
    private final Path canonical, legacy;
    private final Reader reader;
    private final int ownerUid;

    ExtensionConfigStore(Path canonical, Path legacy, Reader reader, int ownerUid) {
        this.canonical = canonical.toAbsolutePath().normalize();
        this.legacy = legacy.toAbsolutePath().normalize();
        this.reader = reader;
        this.ownerUid = ownerUid;
    }

    ExtensionConfig read() throws Exception {
        Path path = Files.exists(canonical, LinkOption.NOFOLLOW_LINKS) ? canonical : legacy;
        if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return ExtensionConfig.defaults();
        return reader.read(path);
    }

    void update(UnaryOperator<ExtensionConfig> change) throws Exception {
        // FileLock handles other processes; the monitor handles overlapping locks in this JVM.
        synchronized (ExtensionConfigStore.class) {
            Path parent = canonical.getParent();
            Files.createDirectories(parent, PosixFilePermissions.asFileAttribute(DIRECTORY_MODE));
            requirePrivate(parent, true);
            Path lock = parent.resolve(".config.lock");
            try {
                Files.createFile(lock, PosixFilePermissions.asFileAttribute(FILE_MODE));
            } catch (FileAlreadyExistsException exists) {
                // Keep this inode: replacing/unlinking it would split the lock between writers.
            }
            requirePrivate(lock, false);
            try (FileChannel channel = FileChannel.open(lock, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
                 FileLock ignored = channel.lock()) {
                write(change.apply(read()));
            }
        }
    }

    private void requirePrivate(Path path, boolean directory) throws IOException {
        BasicFileAttributes attributes = Files.readAttributes(path, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        Set<PosixFilePermission> permissions = directory ? DIRECTORY_MODE : FILE_MODE;
        int uid = ((Number)Files.getAttribute(path, "unix:uid", LinkOption.NOFOLLOW_LINKS)).intValue();
        if (uid != ownerUid || !(directory ? attributes.isDirectory() : attributes.isRegularFile()) ||
            !Files.getPosixFilePermissions(path, LinkOption.NOFOLLOW_LINKS).equals(permissions))
            throw new IOException("unsafe configuration " + (directory ? "directory (requires mode 0700): " : "lock (requires mode 0600): ") + path);
    }

    private void write(ExtensionConfig config) throws Exception {
        Path parent = canonical.getParent();
        Path temporary = Files.createTempFile(parent, ".config-", ".tmp",
            PosixFilePermissions.asFileAttribute(FILE_MODE));
        try {
            Files.write(temporary, config.serialize().getBytes(StandardCharsets.UTF_8));
            Files.move(temporary, canonical, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temporary); }
    }
}
