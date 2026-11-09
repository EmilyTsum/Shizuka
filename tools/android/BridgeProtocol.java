// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.*;
import java.util.*;
import java.util.regex.Pattern;

/** Bounded framing and command validation shared by the local settings bridge. */
final class BridgeProtocol {
    static final int MAX_REQUEST = 16 * 1024, MAX_RESPONSE = 1024 * 1024;
    static final int MAX_OUTPUT = 1024 * 1024, MAX_ARGS = 16, MAX_ARG_BYTES = 256;
    static final long MAX_ARCHIVE = 40L * 1024 * 1024;
    static final String BUSYBOX = "/data/adb/ksu/bin/busybox";
    private static final Set<String> SCRIPTS = Collections.unmodifiableSet(new HashSet<>(
        Arrays.asList("status", "config", "diagnostics", "playback", "control", "apply")));
    private static final String UUID = "[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}";
    private static final Pattern ARCHIVE = Pattern.compile(
        "/data/adb/shizuka/diagnostics/session-" + UUID + "-" + UUID + "\\.zip");
    private BridgeProtocol() {}

    static String readFrame(DataInputStream input, int limit) throws IOException {
        int length = input.readInt();
        if (length <= 0 || length > limit) throw new IOException("Invalid bridge frame length");
        byte[] bytes = new byte[length]; input.readFully(bytes);
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
    }
    static void writeFrame(DataOutputStream output, String text, int limit) throws IOException {
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        if (bytes.length == 0 || bytes.length > limit) throw new IOException("Bridge response exceeds limit");
        output.writeInt(bytes.length); output.write(bytes); output.flush();
    }
    static List<String> command(String module, String script, List<String> args, int timeout) {
        if (!SCRIPTS.contains(script)) throw new IllegalArgumentException("Unsupported bridge command");
        if (timeout < 1 || timeout > 180) throw new IllegalArgumentException("Command timeout must be 1..180 seconds");
        if (args == null || args.size() > MAX_ARGS) throw new IllegalArgumentException("Too many command arguments");
        List<String> command = new ArrayList<>(Arrays.asList(BUSYBOX, "sh", module + "/" + script + ".sh"));
        for (String arg : args) {
            if (arg == null || arg.indexOf('\0') >= 0 || arg.getBytes(StandardCharsets.UTF_8).length > MAX_ARG_BYTES)
                throw new IllegalArgumentException("Invalid bridge command argument");
            command.add(arg);
        }
        return command;
    }
    static String archivePath(String path) {
        if (path == null || !ARCHIVE.matcher(path).matches()) throw new IllegalArgumentException("Invalid diagnostic archive path");
        return path;
    }
    static String processStart(String stat) throws IOException {
        int end = stat.lastIndexOf(") ");
        if (end < 0) throw new IOException("Invalid process identity");
        String[] fields = stat.substring(end + 2).trim().split(" +");
        if (fields.length < 20 || fields[0].equals("Z") || fields[0].equals("X") || !fields[19].matches("[1-9][0-9]*"))
            throw new IOException("Process identity is unavailable");
        return fields[19];
    }
}
