// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Offline replay ingress: timestamp<TAB>base64(message). Never publishes live telemetry. */
public final class DspReplay {
    public static void main(String[] args) throws Exception {
        if (args.length != 0) throw new IllegalArgumentException("stdin replay protocol only");
        DspLogDecoder decoder = new DspLogDecoder(); double last = 0, emitted = -1; long records = 0;
        try (BufferedReader input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
            String line;
            while ((line = input.readLine()) != null) {
                if (line.length() > 32768) throw new IllegalArgumentException("Replay record too large");
                String[] pieces = line.split("\t", 2);
                if (pieces.length != 2) throw new IllegalArgumentException("Invalid replay record");
                last = Double.parseDouble(pieces[0]);
                decoder.accept(last, new String(Base64.getDecoder().decode(pieces[1]), StandardCharsets.UTF_8)); records++;
                if (last - emitted >= 0.25 || last < emitted) { emit(decoder, last, records, false); emitted = last; }
            }
        }
        emit(decoder, last, records, true);
    }
    private static void emit(DspLogDecoder decoder, double now, long records, boolean end) {
        Map<String, Object> out = decoder.snapshot(now);
        out.put("available", false); out.put("replay", true); out.put("live", false);
        out.put("input_records", records); out.put("complete", end);
        System.out.println(ControlJson.encode(out));
    }
}
