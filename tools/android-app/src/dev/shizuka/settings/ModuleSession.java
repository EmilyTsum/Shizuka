// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Map;

/** Fixed root bootstrap boundary. No intent, path or command text enters this command. */
final class ModuleSession {
    static final int TIMEOUT_SECONDS = 45;
    static final int MAX_OUTPUT = 16384;
    static final String MODULE = "/data/adb/modules/s25_aptx_adaptive";
    static String[] command(int uid) {
        if (uid < 10000) throw new IllegalArgumentException("An application UID is required");
        return new String[] {"/system/bin/su", "-c", "/data/adb/ksu/bin/busybox " +
            "timeout -s KILL 15 /data/adb/ksu/bin/busybox sh " + MODULE + "/settings-bootstrap.sh " + uid};
    }
    static boolean validSocket(String value) {
        return value != null && value.matches("shizuka-settings-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");
    }
    static String socket(Map<String, ?> value, int uid) {
        Object schema = value.get("schema"), actual = value.get("companion_uid");
        if (!(schema instanceof Integer || schema instanceof Long) || ((Number)schema).longValue() != 1 ||
            !"settings_session".equals(value.get("scope")) || !Boolean.TRUE.equals(value.get("available")) ||
            !(actual instanceof Integer || actual instanceof Long) || ((Number)actual).longValue() != uid ||
            !(value.get("socket") instanceof String) || !validSocket((String)value.get("socket")))
            throw new IllegalArgumentException("The module returned an invalid connection identity");
        return (String)value.get("socket");
    }
    static String readOutput(InputStream source) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(); byte[] buffer = new byte[1024]; int count;
        while ((count = source.read(buffer)) != -1) {
            if (output.size() + count > MAX_OUTPUT) throw new IOException("Module connection output exceeded its limit");
            output.write(buffer, 0, count);
        }
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(output.toByteArray())).toString();
    }
}
