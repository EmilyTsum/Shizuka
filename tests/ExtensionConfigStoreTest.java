// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.*;
import java.util.concurrent.*;

/** Real file locks/atomic replacement; Android's descriptor reader is a separate boundary. */
public final class ExtensionConfigStoreTest {
    static int assertions;
    static void check(boolean value) { assertions++; if (!value) throw new AssertionError(); }
    static String text(Path path) throws IOException {
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }
    static int uid(Path path) throws IOException {
        return ((Number)Files.getAttribute(path, "unix:uid", LinkOption.NOFOLLOW_LINKS)).intValue();
    }
    static ExtensionConfigStore store(Path root, long delay) throws IOException {
        return new ExtensionConfigStore(root.resolve("config/config.conf"), root.resolve("legacy.conf"), path -> {
            if (delay > 0) Thread.sleep(delay);
            if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) throw new IOException("not a regular config");
            return ExtensionConfig.parse(text(path), false);
        }, uid(root));
    }
    static void await(Path path) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (!Files.exists(path)) {
            if (System.nanoTime() >= deadline) throw new AssertionError("barrier timed out: " + path);
            Thread.sleep(5);
        }
    }
    static void child(String[] args) throws Exception {
        Path root = Paths.get(args[1]);
        ExtensionConfigStore store = store(root, 150);
        Files.createFile(root.resolve(args[2] + ".ready"));
        await(root.resolve("go"));
        if ("migrate".equals(args[2])) store.update(ExtensionConfig::forMigration);
        else store.update(config -> config.with(args[2], args[3]));
    }
    static void concurrentProcesses(Path root) throws Exception {
        Files.write(root.resolve("legacy.conf"), "schema=1\nadaptive_priority=4001\naptx_voice_enabled=false\n".getBytes(StandardCharsets.UTF_8));
        byte[] legacy = Files.readAllBytes(root.resolve("legacy.conf"));
        List<Process> processes = new ArrayList<>();
        String[][] changes = {{"adaptive_priority", "31"}, {"aptx_hd_priority", "32"},
                              {"aptx_voice_enabled", "true"}, {"migrate", ""}};
        String java = Paths.get(System.getProperty("java.home"), "bin/java").toString();
        try {
            for (String[] change : changes) {
                processes.add(new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
                    "ExtensionConfigStoreTest", "child", root.toString(), change[0], change[1])
                    .redirectErrorStream(true).redirectOutput(root.resolve(change[0] + ".log").toFile()).start());
            }
            for (String[] change : changes) await(root.resolve(change[0] + ".ready"));
            Files.createFile(root.resolve("go"));
            for (int i = 0; i < processes.size(); i++) {
                Process process = processes.get(i);
                if (!process.waitFor(10, TimeUnit.SECONDS) || process.exitValue() != 0)
                    throw new AssertionError("concurrent writer failed: " + text(root.resolve(changes[i][0] + ".log")));
            }
            ExtensionConfig result = store(root, 0).read();
            check(result.value("adaptive_priority").equals("31"));
            check(result.value("aptx_hd_priority").equals("32"));
            check(result.value("aptx_voice_enabled").equals("true"));
            check(Arrays.equals(legacy, Files.readAllBytes(root.resolve("legacy.conf"))));
        } finally {
            for (Process process : processes) if (process.isAlive()) process.destroyForcibly().waitFor();
        }
    }
    static void concurrentThreads(Path root) throws Exception {
        ExecutorService workers = Executors.newFixedThreadPool(2);
        CountDownLatch go = new CountDownLatch(1);
        try {
            Future<?> first = workers.submit(() -> {
                try { go.await(); store(root, 100).update(config -> config.with("adaptive_priority", "41")); }
                catch (Exception e) { throw new RuntimeException(e); }
            });
            Future<?> second = workers.submit(() -> {
                try { go.await(); store(root, 100).update(config -> config.with("aptx_hd_priority", "42")); }
                catch (Exception e) { throw new RuntimeException(e); }
            });
            go.countDown();
            first.get(10, TimeUnit.SECONDS); second.get(10, TimeUnit.SECONDS);
            check(store(root, 0).read().value("adaptive_priority").equals("41"));
            check(store(root, 0).read().value("aptx_hd_priority").equals("42"));
        } finally { workers.shutdownNow(); }
    }
    static void fileContracts(Path root) throws Exception {
        ExtensionConfigStore store = store(root, 0);
        Path parent = root.resolve("config"), canonical = parent.resolve("config.conf");
        check(store.read().value("adaptive_priority").equals("system"));
        check(!Files.exists(parent)); // Reads never create a directory or a lock.
        store.update(config -> config.with("adaptive_priority", "7"));
        Path lock = parent.resolve(".config.lock");
        Object inode = Files.readAttributes(lock, BasicFileAttributes.class).fileKey();
        check(Files.getPosixFilePermissions(parent).equals(PosixFilePermissions.fromString("rwx------")));
        check(Files.getPosixFilePermissions(canonical).equals(PosixFilePermissions.fromString("rw-------")));
        byte[] before = Files.readAllBytes(canonical);
        try {
            store.update(config -> { throw new IllegalArgumentException("invalid update"); });
            throw new AssertionError();
        } catch (IllegalArgumentException expected) { check(Arrays.equals(before, Files.readAllBytes(canonical))); }
        store.update(config -> config.with("aptx_hd_priority", "8"));
        check(inode.equals(Files.readAttributes(lock, BasicFileAttributes.class).fileKey()));
        try (java.util.stream.Stream<Path> files = Files.list(parent)) {
            check(files.count() == 2); // Only the canonical file and stable lock remain.
        }
        Files.write(canonical, "schema=invalid\n".getBytes(StandardCharsets.UTF_8));
        try { store.update(ExtensionConfig::forMigration); throw new AssertionError(); }
        catch (IllegalArgumentException expected) { check(text(canonical).equals("schema=invalid\n")); }
        Files.setPosixFilePermissions(parent, PosixFilePermissions.fromString("rwxr-xr-x"));
        try { store.update(ExtensionConfig::forMigration); throw new AssertionError(); }
        catch (IOException expected) { check(expected.getMessage().contains("unsafe configuration directory")); }
        Files.setPosixFilePermissions(parent, PosixFilePermissions.fromString("rwx------"));
        Files.delete(lock);
        Path target = root.resolve("unrelated"); Files.write(target, new byte[] {1});
        Files.createSymbolicLink(lock, target);
        try { store.update(ExtensionConfig::forMigration); throw new AssertionError(); }
        catch (IOException expected) { check(Arrays.equals(Files.readAllBytes(target), new byte[] {1})); }
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 0) { child(args); return; }
        Path root = Files.createTempDirectory("shizuka-store-test-");
        try {
            fileContracts(Files.createDirectory(root.resolve("files")));
            concurrentProcesses(Files.createDirectory(root.resolve("processes")));
            concurrentThreads(Files.createDirectory(root.resolve("threads")));
            System.out.println("Extension store: " + assertions + " checks; concurrent CLI saves/migration, JVM writers, private paths, stable locks and atomic replacement passed.");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path path : (Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator) Files.delete(path);
            }
        }
    }
}
