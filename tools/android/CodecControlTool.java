// SPDX-License-Identifier: GPL-2.0-only
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

/** Host/offline entry point for the same tuning model used by Android BtConfig. */
public final class CodecControlTool {
    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "capabilities".equals(args[0])) {
            System.out.println(ControlJson.encode(CodecTuning.capabilities())); return;
        }
        if (args.length != 7 || !"plan".equals(args[0])) throw new IllegalArgumentException("plan CONFIG PROFILE CODEC MODE RATE BITS");
        Path file = Paths.get(args[1]);
        if (!Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS) || Files.size(file) > 32768)
            throw new IllegalArgumentException("Expected a regular bounded extension configuration");
        ExtensionConfig config = ExtensionConfig.parse(new String(Files.readAllBytes(file), StandardCharsets.UTF_8), false);
        System.out.println(ControlJson.encode(CodecTuning.plan(config, args[2], args[3], args[4], Integer.parseInt(args[5]), Integer.parseInt(args[6]))));
    }
}
