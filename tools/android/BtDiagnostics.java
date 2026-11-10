// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

/** Foreground leased observation, bounded captures and explicit private export. */
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
    private static boolean observationAuthority(Path module, Map<String, Object> bound) throws IOException {
        if (!Files.isRegularFile(module.resolve("module.prop"), LinkOption.NOFOLLOW_LINKS) ||
            Files.exists(module.resolve("disable"), LinkOption.NOFOLLOW_LINKS) ||
            Files.exists(module.resolve("remove"), LinkOption.NOFOLLOW_LINKS) || !bound.equals(identity(module))) return false;
        String status = small(Paths.get("/proc/self/status"));
        boolean root = false, capability = false;
        for (String line : status.split("\\n")) {
            if (line.startsWith("Uid:")) root = line.substring(4).trim().matches("0\\s+0\\s+0\\s+0");
            if (line.startsWith("CapEff:")) try {
                capability = (Long.parseUnsignedLong(line.substring(7).trim(), 16) & (1L << 21)) != 0;
            } catch (NumberFormatException invalid) { return false; }
        }
        return root && capability;
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
        out.put("collected_at_ms", System.currentTimeMillis());
        out.put("binding", "module process identities and DSP rate; not a radio or receiver acknowledgement");
        out.put("android_selection", androidSelection);
        out.put("android_selection_age_seconds", selectionTime < 0 ? null : now() - selectionTime);
        if (!running) out.put("available", false);
        return out;
    }
    private static void captureWithProfiles(Path module, DiagnosticStore store, int seconds, ObservationLease lease) throws Exception {
        BluetoothRuntime runtime = BluetoothRuntime.connect();
        BtStatus.context = runtime.context; BtStatus.adapter = runtime.adapter; BtStatus.moduleRoot = module;
        android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
        BtStatus.profiles = new BluetoothProfiles(runtime,
            new int[] {android.bluetooth.BluetoothProfile.A2DP, android.bluetooth.BluetoothProfile.LE_AUDIO},
            (id, operation, error) -> BtStatus.error("profile " + id + " " + operation, error));
        BtStatus.profiles.start(2500, () -> new Thread(() -> {
            int status = 0;
            try { capture(module, store, seconds, lease); }
            catch (Exception error) { report(error); status = 1; }
            finally { if (lease != null) try { lease.close(); } catch (IOException error) { report(error); } }
            final int result = status;
            handler.post(() -> { BtStatus.profiles.close(); System.exit(result); });
        }, "Shizuka-diagnostics").start());
        android.os.Looper.loop();
    }
    private static void report(Exception error) {
        System.err.println(ControlJson.encode(ControlJson.object("error", error.getClass().getSimpleName(), "reason", String.valueOf(error.getMessage()))));
    }
    private static void capture(Path module, DiagnosticStore store, int seconds, ObservationLease lease) throws Exception {
        if (seconds < 5 || seconds > 120) throw new IllegalArgumentException("Capture duration must be 5..120 seconds");
        if (lease != null && !lease.alive()) {
            System.out.println(ControlJson.encode(ControlJson.object("running", false, "reason", "Foreground observation ended")));
            return;
        }
        Path lock = store.data.resolve(".diagnostics.lock");
        try { store.create(store.data, ".diagnostics.lock"); } catch (FileAlreadyExistsException ignored) {}
        store.check(lock, false);
        try (FileChannel channel = FileChannel.open(lock, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
             FileLock held = channel.tryLock()) {
            if (held == null) throw new IOException("Another diagnostic capture is running");
            Map<String, Object> bound = identity(module);
            if (lease != null && !observationAuthority(module, bound))
                throw new IOException("Foreground observation authority is unavailable");
            String id = "session-" + UUID.randomUUID();
            boolean retain = lease == null;
            Path session = retain ? store.sessions.resolve(id) : null;
            if (retain) store.directory(session);
            Path raw = retain ? store.create(session, "raw.jsonl") : null;
            Path events = retain ? store.create(session, "events.jsonl") : null;
            Path errors = retain ? store.create(session, "native.stderr") : null;
            DspLogDecoder decoder = new DspLogDecoder();
            store.atomic(store.data, "telemetry.json", snapshot(decoder, bound, id, true, "Waiting for encoder records"));
            Process process;
            try {
                ProcessBuilder builder = new ProcessBuilder(module.resolve("dsp-capture").toString(), Integer.toString(seconds));
                if (retain) builder.redirectError(errors.toFile());
                else builder.redirectError(ProcessBuilder.Redirect.INHERIT);
                process = builder.start();
            } catch (IOException error) {
                Map<String, Object> ended = snapshot(decoder, bound, id, false, "Diagnostic reader could not start");
                store.atomic(store.data, "telemetry.json", ended);
                if (retain) store.atomic(session, "summary.json", ended);
                throw error;
            }
            ScheduledExecutorService deadline = Executors.newSingleThreadScheduledExecutor();
            deadline.schedule(process::destroyForcibly, seconds + 10, TimeUnit.SECONDS);
            java.util.concurrent.atomic.AtomicBoolean leaseEnded = new java.util.concurrent.atomic.AtomicBoolean();
            if (lease != null) deadline.scheduleAtFixedRate(() -> {
                boolean alive;
                try { alive = lease.alive() && observationAuthority(module, bound); }
                catch (IOException error) { alive = false; }
                if (!alive && leaseEnded.compareAndSet(false, true)) {
                    process.destroy();
                    try { deadline.schedule(() -> { if (process.isAlive()) process.destroyForcibly(); }, 3, TimeUnit.SECONDS); }
                    catch (RejectedExecutionException ended) { /* Capture has already unwound. */ }
                }
            }, 0, 250, TimeUnit.MILLISECONDS);
            String failure = null; int exit = -1; long bytes = 0, retained = 0;
            double published = -1, historyTime = -1;
            try (BufferedReader input = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
                 BufferedWriter rawOut = retain ? Files.newBufferedWriter(raw, StandardCharsets.UTF_8) : null;
                 BufferedWriter eventOut = retain ? Files.newBufferedWriter(events, StandardCharsets.UTF_8) : null) {
                String line;
                while ((line = input.readLine()) != null) {
                    bytes += line.length() + 1;
                    if (line.length() > 65536) throw new IOException("Diagnostic record exceeds limit");
                    if (retain && retained + line.length() + 1 <= 16 * 1024 * 1024) {
                        rawOut.write(line); rawOut.newLine(); retained += line.length() + 1;
                    }
                    JSONObject record = new JSONObject(line);
                    if ("dsp_log".equals(record.optString("type")))
                        decoder.accept(record.getDouble("monotonic_s"), record.getString("text"));
                    if (now() - published >= 0.25) {
                        if (!bound.equals(identity(module))) throw new IOException("Bluetooth/audio process generation changed");
                        Map<String, Object> value = snapshot(decoder, bound, id, true, "Encoder observation; receiver transmission unverified");
                        store.atomic(store.data, "telemetry.json", value);
                        Map<?, ?> fields = (Map<?, ?>)value.get("fields");
                        double latest = -1;
                        for (String key : new String[] {"encoder_state", "quality_level"}) {
                            Object item = fields.get(key);
                            if (item instanceof Map) {
                                Object time = ((Map<?, ?>)item).get("monotonic_s");
                                if (time instanceof Number) latest = Math.max(latest, ((Number)time).doubleValue());
                            }
                        }
                        if (latest > historyTime) {
                            store.atomic(store.data, "encoder-history.json", value); historyTime = latest;
                        }
                        if (retain) {
                            String encoded = ControlJson.encode(value); eventOut.write(encoded); eventOut.newLine(); eventOut.flush(); rawOut.flush();
                            System.out.println(encoded);
                        }
                        published = now();
                    }
                }
                exit = process.waitFor();
                if (exit != 0 && !leaseEnded.get()) failure = "Native capture exited " + exit + (retain ? "; inspect native.stderr" : "");
            } catch (Exception error) {
                if (!leaseEnded.get()) failure = error.getClass().getSimpleName() + ": " + error.getMessage();
            } finally {
                process.destroy();
                if (!process.waitFor(3, TimeUnit.SECONDS)) process.destroyForcibly();
                deadline.shutdownNow();
                Map<String, Object> ended = snapshot(decoder, bound, id, false, failure != null ? failure :
                    leaseEnded.get() ? "Foreground observation ended" : "Capture ended");
                ended.put("native_exit", exit); ended.put("raw_bytes", retained); ended.put("input_bytes", bytes);
                ended.put("raw_truncated", retain && bytes > retained); ended.put("observation_only", !retain);
                ended.put("artifacts", retain ? Arrays.asList("raw.jsonl", "events.jsonl", "native.stderr") : Collections.emptyList());
                store.atomic(store.data, "telemetry.json", ended);
                if (retain) store.atomic(session, "summary.json", ended);
                System.out.println(ControlJson.encode(ended));
            }
            if (failure != null) throw new IOException(failure);
        }
    }
    public static void main(String[] args) {
        try {
            if (args.length < 2) throw new IllegalArgumentException("module root and action required");
            if ("help".equals(args[1])) {
                System.out.println("diagnostics.sh capture SECONDS|observe TOKEN|renew TOKEN|stop TOKEN|list|export SESSION; foreground observation only, no boot capture or codec writes"); return;
            }
            DiagnosticStore store = new DiagnosticStore(DATA, 0);
            if ("capture".equals(args[1]) && args.length == 3) captureWithProfiles(Paths.get(args[0]), store, Integer.parseInt(args[2]), null);
            else if ("observe".equals(args[1]) && args.length == 3) {
                ObservationLease lease = ObservationLease.begin(store, args[2]);
                try { captureWithProfiles(Paths.get(args[0]), store, 120, lease); }
                catch (Exception error) { lease.close(); throw error; }
            } else if ("renew".equals(args[1]) && args.length == 3)
                System.out.println(ControlJson.encode(ControlJson.object("running", ObservationLease.renew(store, args[2]))));
            else if ("stop".equals(args[1]) && args.length == 3)
                System.out.println(ControlJson.encode(ControlJson.object("stopped", ObservationLease.stop(store, args[2]))));
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
