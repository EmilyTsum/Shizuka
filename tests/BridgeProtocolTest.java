// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public final class BridgeProtocolTest {
    private static int assertions;
    interface Action { void run() throws Exception; }
    static void check(boolean condition) {
        assertions++;
        if (!condition) throw new AssertionError("check " + assertions);
    }
    static void rejects(Action action) throws Exception {
        assertions++;
        try { action.run(); }
        catch (IOException | IllegalArgumentException expected) { return; }
        throw new AssertionError("Invalid bridge input accepted at check " + assertions);
    }
    static DataInputStream frame(int length, byte[] bytes) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        DataOutputStream data = new DataOutputStream(out); data.writeInt(length); data.write(bytes);
        return new DataInputStream(new ByteArrayInputStream(out.toByteArray()));
    }
    static List<String> command(String script, List<String> args, int timeout) {
        return BridgeProtocol.command("/module", script, args, timeout);
    }
    public static void main(String[] ignored) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        String text = "{\"kind\":\"call\",\"name\":\"音声\"}";
        BridgeProtocol.writeFrame(new DataOutputStream(out), text, BridgeProtocol.MAX_REQUEST);
        byte[] encoded = out.toByteArray();
        check(new DataInputStream(new ByteArrayInputStream(encoded)).readInt() == text.getBytes(StandardCharsets.UTF_8).length);
        check(BridgeProtocol.readFrame(new DataInputStream(new ByteArrayInputStream(encoded)), BridgeProtocol.MAX_REQUEST).equals(text));
        rejects(() -> BridgeProtocol.readFrame(frame(-1, new byte[0]), BridgeProtocol.MAX_REQUEST));
        rejects(() -> BridgeProtocol.readFrame(frame(0, new byte[0]), BridgeProtocol.MAX_REQUEST));
        rejects(() -> BridgeProtocol.readFrame(frame(BridgeProtocol.MAX_REQUEST + 1, new byte[0]), BridgeProtocol.MAX_REQUEST));
        rejects(() -> BridgeProtocol.readFrame(frame(4, new byte[] {1, 2}), BridgeProtocol.MAX_REQUEST));
        rejects(() -> BridgeProtocol.readFrame(frame(2, new byte[] {(byte)0xc3, 0x28}), BridgeProtocol.MAX_REQUEST));
        rejects(() -> BridgeProtocol.writeFrame(new DataOutputStream(new ByteArrayOutputStream()), "oversize", 2));
        rejects(() -> BridgeProtocol.writeFrame(new DataOutputStream(new ByteArrayOutputStream()), "", 2));
        for (String script : Arrays.asList("status", "config", "diagnostics", "playback", "control", "apply"))
            check(command(script, Collections.emptyList(), 1).equals(Arrays.asList(BridgeProtocol.BUSYBOX, "sh", "/module/" + script + ".sh")));
        List<String> literal = command("config", Arrays.asList("a; rm -rf /", "$(id)", "a\nb"), 180);
        check(literal.get(3).equals("a; rm -rf /")); // Arguments remain values, never a shell command string.
        check(literal.get(4).equals("$(id)"));
        check(literal.get(5).equals("a\nb"));
        rejects(() -> command("sh", Collections.emptyList(), 10));
        rejects(() -> command("../control", Collections.emptyList(), 10));
        rejects(() -> command("control.sh", Collections.emptyList(), 10));
        rejects(() -> command("status", Collections.emptyList(), 0));
        rejects(() -> command("status", Collections.emptyList(), 181));
        rejects(() -> command("status", Collections.nCopies(17, "x"), 10));
        check(command("status", Collections.nCopies(16, "x"), 10).size() == 19);
        check(command("config", Arrays.asList(String.join("", Collections.nCopies(256, "x"))), 10).get(3).length() == 256);
        rejects(() -> command("config", Arrays.asList(String.join("", Collections.nCopies(257, "x"))), 10));
        rejects(() -> command("config", Arrays.asList(String.join("", Collections.nCopies(86, "声"))), 10));
        rejects(() -> command("config", Arrays.asList("nul\0arg"), 10));
        rejects(() -> command("config", Arrays.asList((String)null), 10));
        String id = "11111111-2222-3333-4444-555555555555";
        String archive = "/data/adb/shizuka/diagnostics/session-" + id + "-" + id + ".zip";
        check(BridgeProtocol.archivePath(archive).equals(archive));
        rejects(() -> BridgeProtocol.archivePath(archive.replace("diagnostics/", "diagnostics/../")));
        rejects(() -> BridgeProtocol.archivePath(archive + "/extra"));
        rejects(() -> BridgeProtocol.archivePath(archive.replace("/data/adb/shizuka/", "/sdcard/")));
        rejects(() -> BridgeProtocol.archivePath(archive.replace(id + "-" + id, id)));
        rejects(() -> BridgeProtocol.archivePath(archive.replace(".zip", ".txt")));
        String stat = "123 (command ) containing spaces) S" + String.join("", Collections.nCopies(18, " 0")) + " 987";
        check(BridgeProtocol.processStart(stat).equals("987"));
        rejects(() -> BridgeProtocol.processStart("123 (short) S 1 2 3"));
        rejects(() -> BridgeProtocol.processStart(stat.replace(") S", ") Z")));
        rejects(() -> BridgeProtocol.processStart(stat.replace("987", "-1")));
        System.out.println("Settings bridge: " + assertions + " checks; bounded UTF-8 framing, fixed scripts/literal argv, archive paths, timeouts and process identities passed.");
    }
}
