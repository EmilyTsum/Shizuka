// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/** Fixed module APIs over the temporary root session opened by KernelSU Action. */
final class RootApi {
    static final String MODULE = "/data/adb/modules/s25_aptx_adaptive";
    private static final int MAX_HEADER = 1024 * 1024;
    private static final long MAX_ARCHIVE = 40L * 1024 * 1024;
    volatile String lastOutput = "No command has run yet.";
    private volatile String socketName;
    private volatile long generation;

    static final class Unavailable extends IllegalStateException {
        final String kind;
        Unavailable(String kind, String message) { super(message); this.kind = kind; }
    }
    void bind(String socket) {
        socketName = socket != null && socket.matches("shizuka-settings-[a-f0-9-]{36}") ? socket : null;
        generation++;
    }
    long generation() { return generation; }
    private static Unavailable ended() {
        return new Unavailable("session", "Open Shizuka again from KernelSU Action. The root session has ended.");
    }
    private LocalSocket connect(int timeout) throws Exception {
        String name = socketName;
        if (name == null) throw ended();
        LocalSocket socket = new LocalSocket();
        try {
            socket.connect(new LocalSocketAddress(name, LocalSocketAddress.Namespace.ABSTRACT));
            socket.setSoTimeout((timeout + 10) * 1000);
            if (socket.getPeerCredentials() == null || socket.getPeerCredentials().getUid() != 0)
                throw new Unavailable("session", "The module session could not be authenticated. Open Shizuka again from KernelSU Action.");
            return socket;
        } catch (Exception error) {
            try { socket.close(); } catch (Exception ignored) { }
            if (error instanceof Unavailable) throw error;
            throw ended();
        }
    }
    private static void send(DataOutputStream output, JSONObject request) throws Exception {
        byte[] bytes = request.toString().getBytes(StandardCharsets.UTF_8);
        if (bytes.length > 16384) throw new IllegalArgumentException("Module request exceeds its limit");
        output.writeInt(bytes.length);
        output.write(bytes);
        output.flush();
    }
    private static JSONObject header(DataInputStream input) throws Exception {
        int size = input.readInt();
        if (size < 2 || size > MAX_HEADER) throw new IllegalStateException("Invalid module response length");
        byte[] bytes = new byte[size];
        input.readFully(bytes);
        return new JSONObject(new String(bytes, StandardCharsets.UTF_8));
    }
    private static void requireOk(JSONObject response) throws Exception {
        if (!response.optBoolean("ok")) {
            String text = response.optString("output", response.optString("error", "The module request was rejected."));
            if (text.contains("module_unavailable") || text.contains("module is unavailable"))
                throw new Unavailable("module", "The Shizuka KernelSU module is missing, disabled or marked for removal. Reopen its module Action after enabling it.");
            throw new IllegalStateException(text);
        }
    }

    String call(String script, int timeout, String... arguments) throws Exception {
        if (!script.matches("status|config|diagnostics|playback|control|apply"))
            throw new IllegalArgumentException("Unknown module API");
        if (timeout < 1 || timeout > 180 || arguments.length > 16)
            throw new IllegalArgumentException("Invalid module request");
        JSONArray args = new JSONArray();
        for (String argument : arguments) {
            if (argument.length() > 1024) throw new IllegalArgumentException("Module argument exceeds its limit");
            args.put(argument);
        }
        JSONObject request = new JSONObject().put("kind", "call").put("script", script)
            .put("args", args).put("timeout", timeout);
        try (LocalSocket socket = connect(timeout)) {
            DataOutputStream output = new DataOutputStream(socket.getOutputStream());
            DataInputStream input = new DataInputStream(socket.getInputStream());
            send(output, request);
            JSONObject response = header(input);
            requireOk(response);
            String text = response.optString("output", "").trim();
            lastOutput = script + ".sh " + String.join(" ", arguments) + "\n\n" + text;
            if (response.optInt("exit", 1) != 0) throw new IllegalStateException(text.isEmpty()
                ? "The module command did not complete successfully." : text);
            return text;
        } catch (java.io.IOException closed) { throw ended(); }
    }

    JSONObject json(String script, int timeout, String... arguments) throws Exception {
        String output = call(script, timeout, arguments);
        String[] lines = output.split("\\r?\\n");
        for (int index = lines.length - 1; index >= 0; index--) {
            if (lines[index].trim().startsWith("{")) {
                try { return new JSONObject(lines[index]); } catch (Exception ignored) { }
            }
        }
        throw new IllegalStateException("The module returned no structured result. Update the Shizuka module.");
    }

    void copyArchive(String archive, OutputStream target) throws Exception {
        if (!archive.matches("/data/adb/shizuka/diagnostics/session-[a-f0-9-]{36}-[a-f0-9-]{36}\\.zip"))
            throw new IllegalArgumentException("Unexpected diagnostic archive path");
        try (LocalSocket socket = connect(60)) {
            DataOutputStream output = new DataOutputStream(socket.getOutputStream());
            DataInputStream input = new DataInputStream(socket.getInputStream());
            send(output, new JSONObject().put("kind", "archive").put("path", archive));
            JSONObject response = header(input);
            requireOk(response);
            long left = response.optLong("size", -1);
            if (left < 1 || left > MAX_ARCHIVE) throw new IllegalStateException("Invalid diagnostic archive size");
            byte[] buffer = new byte[16384];
            while (left > 0) {
                int count = input.read(buffer, 0, (int)Math.min(buffer.length, left));
                if (count < 0) throw new IllegalStateException("The diagnostic archive was interrupted");
                target.write(buffer, 0, count);
                left -= count;
            }
        } catch (java.io.IOException closed) { throw ended(); }
    }
}
