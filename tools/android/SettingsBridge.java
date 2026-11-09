// SPDX-License-Identifier: GPL-2.0-only
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.json.*;

/** Module-owned, UID-authenticated local bridge. Created only by a root Action. */
public final class SettingsBridge implements AutoCloseable {
    private static final long IDLE_NANOS = TimeUnit.MINUTES.toNanos(15), LIFE_NANOS = TimeUnit.HOURS.toNanos(2);
    private final Path module, state, ready;
    private final String socketName, start;
    private final int uid, pid;
    private final long moduleInode, moduleDevice, born = System.nanoTime();
    private final AtomicLong lastUse = new AtomicLong(born);
    private final AtomicBoolean closed = new AtomicBoolean();
    private final Set<LocalSocket> clients = ConcurrentHashMap.newKeySet();
    private final Set<Process> children = ConcurrentHashMap.newKeySet();
    private final ScheduledExecutorService deadlines = Executors.newSingleThreadScheduledExecutor();
    private final ThreadPoolExecutor workers = new ThreadPoolExecutor(3, 3, 0, TimeUnit.SECONDS,
        new ArrayBlockingQueue<Runnable>(6));
    private final LocalServerSocket server;

    private SettingsBridge(Path module, String socketName, int uid) throws Exception {
        if (Os.getuid() != 0 || uid < 10000) throw new IllegalArgumentException("Root and a companion app UID are required");
        if (!socketName.matches("shizuka-settings-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}"))
            throw new IllegalArgumentException("Invalid bridge socket name");
        this.module = module.toRealPath();
        StructStat directory = Os.lstat(this.module.toString());
        if (!OsConstants.S_ISDIR(directory.st_mode) || directory.st_uid != 0)
            throw new IOException("Module directory is not root-owned");
        moduleInode = directory.st_ino; moduleDevice = directory.st_dev;
        this.socketName = socketName; this.uid = uid; pid = Os.getpid();
        start = BridgeProtocol.processStart(new String(Files.readAllBytes(Paths.get("/proc/self/stat")), StandardCharsets.UTF_8));
        state = this.module.resolve("state"); ready = state.resolve("settings-bridge.json");
        Files.createDirectories(state, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        if (Files.isSymbolicLink(state) || ((Number)Files.getAttribute(state, "unix:uid", LinkOption.NOFOLLOW_LINKS)).intValue() != 0)
            throw new IOException("Unsafe bridge state directory");
        Files.setPosixFilePermissions(state, PosixFilePermissions.fromString("rwx------"));
        if (!modulePresent()) throw new IOException("Module is disabled or removed");
        server = new LocalServerSocket(socketName);
    }
    private boolean modulePresent() {
        try {
            StructStat value = Os.lstat(module.toString());
            return OsConstants.S_ISDIR(value.st_mode) && value.st_uid == 0 && value.st_ino == moduleInode && value.st_dev == moduleDevice &&
                Files.isRegularFile(module.resolve("module.prop"), LinkOption.NOFOLLOW_LINKS) &&
                !Files.exists(module.resolve("disable"), LinkOption.NOFOLLOW_LINKS) &&
                !Files.exists(module.resolve("remove"), LinkOption.NOFOLLOW_LINKS);
        } catch (Exception unavailable) { return false; }
    }
    private void publish() throws IOException {
        Path temp = Files.createTempFile(state, ".settings-bridge-", ".tmp",
            PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            String json = ControlJson.encode(ControlJson.object("pid", pid, "start", start,
                "socket", socketName, "companion_uid", uid));
            Files.write(temp, (json + "\n").getBytes(StandardCharsets.UTF_8));
            Files.move(temp, ready, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temp); }
    }
    private void serve() throws Exception {
        publish();
        deadlines.scheduleAtFixedRate(() -> {
            long now = System.nanoTime();
            if (!modulePresent() || now - born >= LIFE_NANOS || now - lastUse.get() >= IDLE_NANOS) close();
        }, 1, 1, TimeUnit.SECONDS);
        while (!closed.get()) {
            LocalSocket socket;
            try { socket = server.accept(); }
            catch (IOException error) { if (closed.get()) break; throw error; }
            try {
                if (socket.getPeerCredentials().getUid() != uid || !modulePresent()) { socket.close(); continue; }
                socket.setSoTimeout(5000);
                clients.add(socket);
                ScheduledFuture<?> limit = deadlines.schedule(() -> closeClient(socket), 190, TimeUnit.SECONDS);
                try { workers.execute(() -> handle(socket, limit)); }
                catch (RejectedExecutionException busy) { limit.cancel(false); closeClient(socket); }
            } catch (Exception error) { closeClient(socket); }
        }
    }
    private static final class Response {
        final DataOutputStream output;
        boolean started;
        Response(OutputStream output) { this.output = new DataOutputStream(output); }
        void header(Object value) throws IOException {
            started = true;
            BridgeProtocol.writeFrame(output, ControlJson.encode(value), BridgeProtocol.MAX_RESPONSE);
        }
    }
    private void handle(LocalSocket socket, ScheduledFuture<?> limit) {
        Response response = null;
        try {
            if (!modulePresent() || socket.getPeerCredentials().getUid() != uid) throw new IOException("Bridge is unavailable");
            response = new Response(socket.getOutputStream());
            JSONObject request = new JSONObject(BridgeProtocol.readFrame(new DataInputStream(socket.getInputStream()), BridgeProtocol.MAX_REQUEST));
            lastUse.set(System.nanoTime());
            String kind = string(request, "kind");
            if ("call".equals(kind)) call(request, response);
            else if ("archive".equals(kind)) archive(string(request, "path"), response);
            else throw new IllegalArgumentException("Unsupported bridge request");
        } catch (Exception error) {
            if (response != null && !response.started) try {
                response.header(ControlJson.object("ok", false, "exit", 1,
                    "output", error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage()));
            } catch (IOException ignored) {}
        } finally {
            limit.cancel(false); closeClient(socket); lastUse.set(System.nanoTime());
        }
    }
    private static String string(JSONObject object, String key) throws JSONException {
        Object value = object.get(key);
        if (!(value instanceof String)) throw new IllegalArgumentException("Expected string: " + key);
        return (String)value;
    }
    private void call(JSONObject request, Response response) throws Exception {
        String script = string(request, "script");
        JSONArray array = request.getJSONArray("args");
        if (array.length() > BridgeProtocol.MAX_ARGS) throw new IllegalArgumentException("Too many command arguments");
        List<String> args = new ArrayList<>();
        for (int i = 0; i < array.length(); i++) {
            Object arg = array.get(i);
            if (!(arg instanceof String)) throw new IllegalArgumentException("Command arguments must be strings");
            args.add((String)arg);
        }
        Object seconds = request.get("timeout");
        if (!(seconds instanceof Number) || ((Number)seconds).doubleValue() != ((Number)seconds).intValue())
            throw new IllegalArgumentException("Command timeout must be an integer");
        int timeout = ((Number)seconds).intValue();
        List<String> command = BridgeProtocol.command(module.toString(), script, args, timeout);
        Path file = module.resolve(script + ".sh");
        StructStat stat = Os.lstat(file.toString());
        if (!OsConstants.S_ISREG(stat.st_mode) || stat.st_uid != 0 || !modulePresent())
            throw new IOException("Module command is unavailable");
        Process process = new ProcessBuilder(command).directory(module.toFile()).redirectErrorStream(true).start();
        children.add(process);
        process.getOutputStream().close(); // Fixed CLI commands never receive interactive input.
        OutputCapture output = new OutputCapture(process.getInputStream());
        Thread drain = new Thread(output, "settings-command-output"); drain.setDaemon(true); drain.start();
        boolean ended = false;
        try {
            ended = process.waitFor(timeout, TimeUnit.SECONDS);
            if (!ended) { process.destroyForcibly(); process.waitFor(2, TimeUnit.SECONDS); }
            drain.join(2000);
            if (drain.isAlive()) { process.getInputStream().close(); drain.join(1000); }
            int exit = ended ? process.exitValue() : 124;
            String text = output.text() + (ended ? "" : "\nCommand timed out.");
            Map<String, Object> result = ControlJson.object("ok", exit == 0, "exit", exit, "output", text);
            while (ControlJson.encode(result).getBytes(StandardCharsets.UTF_8).length > BridgeProtocol.MAX_RESPONSE) {
                text = "[earlier output truncated]\n" + text.substring(text.length() / 2);
                result.put("output", text);
            }
            response.header(result);
        } finally {
            if (!ended) process.destroyForcibly();
            try { process.getInputStream().close(); } catch (IOException ignored) {}
            children.remove(process);
        }
    }
    private static final class OutputCapture implements Runnable {
        private final InputStream input;
        private final byte[] tail = new byte[BridgeProtocol.MAX_OUTPUT];
        private int next, length;
        private boolean truncated;
        OutputCapture(InputStream input) { this.input = input; }
        public void run() {
            byte[] buffer = new byte[8192];
            try {
                int count;
                while ((count = input.read(buffer)) != -1) append(buffer, count);
            } catch (IOException ignored) { /* Timeout/bridge shutdown closes the owned pipe. */ }
        }
        private synchronized void append(byte[] buffer, int count) {
            int first = Math.min(count, tail.length - next);
            System.arraycopy(buffer, 0, tail, next, first);
            if (first < count) System.arraycopy(buffer, first, tail, 0, count - first);
            next = (next + count) % tail.length;
            if (length + count > tail.length) truncated = true;
            length = Math.min(tail.length, length + count);
        }
        synchronized String text() {
            byte[] bytes = new byte[length];
            int begin = (next - length + tail.length) % tail.length, first = Math.min(length, tail.length - begin);
            System.arraycopy(tail, begin, bytes, 0, first);
            if (first < length) System.arraycopy(tail, 0, bytes, first, length - first);
            return (truncated ? "[earlier output truncated]\n" : "") + new String(bytes, StandardCharsets.UTF_8);
        }
    }
    private void archive(String value, Response response) throws Exception {
        String path = BridgeProtocol.archivePath(value);
        for (String directory : Arrays.asList("/data/adb", "/data/adb/shizuka", "/data/adb/shizuka/diagnostics")) {
            StructStat stat = Os.lstat(directory);
            if (!OsConstants.S_ISDIR(stat.st_mode) || stat.st_uid != 0) throw new IOException("Unsafe archive directory");
        }
        java.io.FileDescriptor fd = Os.open(path, OsConstants.O_RDONLY | OsConstants.O_CLOEXEC | OsConstants.O_NOFOLLOW, 0);
        try (FileInputStream input = new FileInputStream(fd)) {
            StructStat stat = Os.fstat(fd);
            if (!OsConstants.S_ISREG(stat.st_mode) || stat.st_uid != 0 || stat.st_size < 0 || stat.st_size > BridgeProtocol.MAX_ARCHIVE)
                throw new IOException("Unsafe or oversized diagnostic archive");
            response.header(ControlJson.object("ok", true, "size", stat.st_size));
            byte[] buffer = new byte[32768]; long remaining = stat.st_size;
            while (remaining > 0) {
                if (!modulePresent() || closed.get()) throw new IOException("Module was removed");
                int count = input.read(buffer, 0, (int)Math.min(buffer.length, remaining));
                if (count < 0) throw new EOFException("Diagnostic archive was truncated");
                response.output.write(buffer, 0, count); remaining -= count;
            }
            response.output.flush();
        }
    }
    private void closeClient(LocalSocket socket) {
        clients.remove(socket);
        try { socket.close(); } catch (IOException ignored) {}
    }
    public void close() {
        if (!closed.compareAndSet(false, true)) return;
        try { server.close(); } catch (IOException ignored) {}
        for (LocalSocket socket : clients) closeClient(socket);
        for (Process child : children) child.destroyForcibly();
        workers.shutdownNow(); deadlines.shutdownNow();
        try {
            if (Files.isRegularFile(ready, LinkOption.NOFOLLOW_LINKS) && Files.size(ready) <= 4096) {
                JSONObject record = new JSONObject(new String(Files.readAllBytes(ready), StandardCharsets.UTF_8));
                if (record.optInt("pid") == pid && start.equals(record.optString("start")) && socketName.equals(record.optString("socket")))
                    Files.deleteIfExists(ready);
            }
        } catch (Exception ignored) { /* A newer bridge owns a replacement record. */ }
    }
    public static void main(String[] args) {
        try {
            if (args.length != 3) throw new IllegalArgumentException("Module path, socket and companion UID are required");
            int uid = Integer.parseInt(args[2]);
            try (SettingsBridge bridge = new SettingsBridge(Paths.get(args[0]), args[1], uid)) {
                Runtime.getRuntime().addShutdownHook(new Thread(bridge::close));
                bridge.serve();
            }
        } catch (Exception error) { System.err.println("settings_bridge_error=" + error.getMessage()); System.exit(1); }
    }
}
