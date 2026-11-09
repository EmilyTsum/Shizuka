// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

/** Foreground opt-in capture, structured live snapshots and explicit private export. */
public final class BtDiagnostics {
    private static final Path DATA = Paths.get("/data/adb/shizuka");
    private static Object androidSelection = Collections.emptyMap();
    private static double selectionTime = -1;
    private static String selectionFingerprint;
    private static Object plain(Object value) throws Exception {
        if (value == JSONObject.NULL) return null;
        if (value instanceof JSONObject) {
            Map<String, Object> out = new LinkedHashMap<>();
            JSONObject object = (JSONObject)value;
            for (Iterator<String> keys = object.keys(); keys.hasNext();) {
                String key = keys.next(); out.put(key, plain(object.get(key)));
            }
            return out;
        }
        if (value instanceof JSONArray) {
            List<Object> out = new ArrayList<>(); JSONArray array = (JSONArray)value;
            for (int i = 0; i < array.length(); i++) out.add(plain(array.get(i)));
            return out;
        }
        return value;
    }
    private static double now() { return System.nanoTime() / 1e9; }
    private static String small(Path path) throws IOException {
        if (!Files.isRegularFile(path) || Files.size(path) > 8192) throw new IOException("Missing identity: " + path.getFileName());
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8).trim();
    }
    static String start(String pid) throws IOException {
        if (!pid.matches("[1-9][0-9]*")) throw new IOException("Invalid process identity");
        String stat = small(Paths.get("/proc", pid, "stat"));
        int end = stat.lastIndexOf(") ");
        if (end < 0) throw new IOException("Invalid process stat");
        String[] fields = stat.substring(end + 2).split(" +");
        if (fields.length < 20 || fields[0].equals("Z") || fields[0].equals("X")) throw new IOException("Process is gone");
        return fields[19];
    }
    private static Map<String, Object> identity(Path module) throws IOException {
        String bt = small(module.resolve("state/bt.pid")), hal = small(module.resolve("state/hal.pid"));
        String btStart = small(module.resolve("state/bt.start")), halStart = small(module.resolve("state/hal.start"));
        if (!Files.isRegularFile(module.resolve("state/active")) ||
            !start(bt).equals(btStart) || !start(hal).equals(halStart))
            throw new IOException("Module process generation is not verified; apply/repair the module first");
        return ControlJson.object("boot_id", small(Paths.get("/proc/sys/kernel/random/boot_id")),
            "bluetooth_pid", bt, "bluetooth_start", btStart, "hal_pid", hal, "hal_start", halStart);
    }
    private static Map<String, Object> snapshot(DspLogDecoder decoder, Map<String, Object> identity, String id, boolean running, String reason) {
        if (running && now() - selectionTime >= 1) {
            try {
                JSONObject selection = BtStatus.selectionSnapshot();
                JSONObject fingerprint = new JSONObject(selection.toString());
                fingerprint.remove("collected_at_ms");
                fingerprint.remove("tuning");
                JSONArray devices = fingerprint.getJSONArray("devices");
                for (int i = 0; i < devices.length(); i++) devices.getJSONObject(i).remove("stream_started");
                String encoded = ControlJson.encode(plain(fingerprint));
                if (selectionFingerprint != null && !selectionFingerprint.equals(encoded)) decoder.contextChanged("Android selection or media route changed");
                selectionFingerprint = encoded; androidSelection = plain(selection);
            }
            catch (Exception error) { androidSelection = ControlJson.object("available", false, "reason", error.getClass().getSimpleName()); }
            selectionTime = now();
        }
        Map<String, Object> out = decoder.snapshot(now()); out.putAll(identity);
        out.put("capture_id", id); out.put("running", running); out.put("reason", reason);
        out.put("binding", "module process identities and DSP rate; not a radio or receiver acknowledgement");
        out.put("android_selection", androidSelection);
        out.put("android_selection_age_seconds", selectionTime < 0 ? null : now() - selectionTime);
        if (!running) out.put("available", false);
        return out;
    }
    private static void captureWithProfiles(Path module, DiagnosticStore store, int seconds) throws Exception {
        BluetoothRuntime runtime = BluetoothRuntime.connect();
        BtStatus.context = runtime.context; BtStatus.adapter = runtime.adapter; BtStatus.moduleRoot = module;
        android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
        BtStatus.profiles = new BluetoothProfiles(runtime,
            new int[] {android.bluetooth.BluetoothProfile.A2DP, android.bluetooth.BluetoothProfile.LE_AUDIO},
            (id, operation, error) -> BtStatus.error("profile " + id + " " + operation, error));
        BtStatus.profiles.start(2500, () -> new Thread(() -> {
            int status = 0;
            try { capture(module, store, seconds); }
            catch (Exception error) { report(error); status = 1; }
            final int result = status;
            handler.post(() -> { BtStatus.profiles.close(); System.exit(result); });
        }, "Shizuka-diagnostics").start());
        android.os.Looper.loop();
    }
    private static void report(Exception error) {
        System.err.println(ControlJson.encode(ControlJson.object("error", error.getClass().getSimpleName(), "reason", String.valueOf(error.getMessage()))));
    }
    private static void capture(Path module, DiagnosticStore store, int seconds) throws Exception {
        if (seconds < 5 || seconds > 120) throw new IllegalArgumentException("Capture duration must be 5..120 seconds");
        Path lock = store.data.resolve(".diagnostics.lock");
        try { store.create(store.data, ".diagnostics.lock"); } catch (FileAlreadyExistsException ignored) {}
        store.check(lock, false);
        try (FileChannel channel = FileChannel.open(lock, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
             FileLock held = channel.tryLock()) {
            if (held == null) throw new IOException("Another diagnostic capture is running");
            Map<String, Object> bound = identity(module);
            String id = "session-" + UUID.randomUUID();
            Path session = store.sessions.resolve(id); store.directory(session);
            Path raw = store.create(session, "raw.jsonl"), events = store.create(session, "events.jsonl");
            Path errors = store.create(session, "native.stderr");
            DspLogDecoder decoder = new DspLogDecoder();
            store.atomic(store.data, "telemetry.json", snapshot(decoder, bound, id, true, "Waiting for encoder records"));
            Process process;
            try {
                process = new ProcessBuilder(module.resolve("dsp-capture").toString(), Integer.toString(seconds))
                    .redirectError(errors.toFile()).start();
            } catch (IOException error) {
                Map<String, Object> ended = snapshot(decoder, bound, id, false, "Diagnostic reader could not start");
                store.atomic(store.data, "telemetry.json", ended); store.atomic(session, "summary.json", ended);
                throw error;
            }
            ScheduledExecutorService deadline = Executors.newSingleThreadScheduledExecutor();
            deadline.schedule(process::destroyForcibly, seconds + 10, TimeUnit.SECONDS);
            String failure = null; int exit = -1; long bytes = 0; double published = -1;
            try (BufferedReader input = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
                 BufferedWriter rawOut = Files.newBufferedWriter(raw, StandardCharsets.UTF_8);
                 BufferedWriter eventOut = Files.newBufferedWriter(events, StandardCharsets.UTF_8)) {
                String line;
                while ((line = input.readLine()) != null) {
                    bytes += line.length() + 1;
                    if (line.length() > 65536 || bytes > 16 * 1024 * 1024) throw new IOException("Capture limit reached");
                    rawOut.write(line); rawOut.newLine();
                    JSONObject record = new JSONObject(line);
                    if ("dsp_log".equals(record.optString("type")))
                        decoder.accept(record.getDouble("monotonic_s"), record.getString("text"));
                    if (now() - published >= 0.25) {
                        if (!bound.equals(identity(module))) throw new IOException("Bluetooth/audio process generation changed");
                        Map<String, Object> value = snapshot(decoder, bound, id, true, "Encoder observation; receiver transmission unverified");
                        store.atomic(store.data, "telemetry.json", value);
                        String encoded = ControlJson.encode(value); eventOut.write(encoded); eventOut.newLine(); eventOut.flush(); rawOut.flush();
                        System.out.println(encoded); published = now();
                    }
                }
                exit = process.waitFor();
                if (exit != 0) failure = "Native capture exited " + exit + "; inspect native.stderr";
            } catch (Exception error) {
                failure = error.getClass().getSimpleName() + ": " + error.getMessage();
            } finally {
                process.destroy();
                if (!process.waitFor(3, TimeUnit.SECONDS)) process.destroyForcibly();
                deadline.shutdownNow();
                Map<String, Object> ended = snapshot(decoder, bound, id, false, failure == null ? "Capture ended" : failure);
                ended.put("native_exit", exit); ended.put("raw_bytes", bytes);
                ended.put("artifacts", Arrays.asList("raw.jsonl", "events.jsonl", "native.stderr"));
                store.atomic(store.data, "telemetry.json", ended); store.atomic(session, "summary.json", ended);
                System.out.println(ControlJson.encode(ended));
            }
            if (failure != null) throw new IOException(failure);
        }
    }
    public static void main(String[] args) {
        try {
            if (args.length < 2) throw new IllegalArgumentException("module root and action required");
            if ("help".equals(args[1])) {
                System.out.println("diagnostics.sh capture SECONDS|list|export SESSION; no boot capture, properties or codec writes"); return;
            }
            DiagnosticStore store = new DiagnosticStore(DATA, 0);
            if ("capture".equals(args[1]) && args.length == 3) captureWithProfiles(Paths.get(args[0]), store, Integer.parseInt(args[2]));
            else if ("export".equals(args[1]) && args.length == 3)
                System.out.println(ControlJson.encode(ControlJson.object("schema", 1, "archive", store.export(args[2]).toString())));
            else if ("list".equals(args[1]) && args.length == 2) {
                List<String> sessions = new ArrayList<>();
                try (DirectoryStream<Path> paths = Files.newDirectoryStream(store.sessions, "session-*")) {
                    for (Path path : paths) if (Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) sessions.add(path.getFileName().toString());
                }
                Collections.sort(sessions); System.out.println(ControlJson.encode(ControlJson.object("schema", 1, "sessions", sessions)));
            } else throw new IllegalArgumentException("Invalid diagnostics command");
        } catch (Exception error) {
            report(error);
            System.exit(1);
        }
    }
}
