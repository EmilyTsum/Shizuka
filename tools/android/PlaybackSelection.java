// SPDX-License-Identifier: GPL-2.0-only
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.regex.*;

/** Portable request validation and reflective codec planning; no Android service access or writes. */
final class PlaybackSelection {
    private PlaybackSelection() {}
    static final String USAGE = "playback.sh get [ADDRESS|active] | select ADDRESS|active adaptive hq|ll RATE BITS | select ADDRESS|active lossless hq 44100 16";
    // aptX Adaptive selection contract. Codec IDs come from peer capabilities.
    static final long ADAPTIVE_HQ = 4096, ADAPTIVE_LL = 8192;

    private static Object call(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        try { return target.getClass().getMethod(name, types).invoke(target, args); }
        catch (NoSuchMethodException error) {
            throw new UnsupportedOperationException("Codec API unavailable: " + name, error);
        } catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            if (cause instanceof Error) throw (Error)cause;
            throw new IllegalStateException("Codec API failed: " + name, cause);
        }
    }
    static Object get(Object object, String method) throws Exception {
        return call(object, method, new Class<?>[] {});
    }
    static int number(Object object, String method) throws Exception {
        return ((Number)get(object, method)).intValue();
    }
    static Object extendedType(Object codec) throws Exception {
        try { return get(codec, "getExtendedCodecType"); }
        catch (UnsupportedOperationException unavailable) { return null; }
    }
    static String name(Object codec) throws Exception {
        Object type = extendedType(codec);
        if (type != null) {
            Object value = get(type, "getCodecName");
            if (value instanceof String && !((String)value).trim().isEmpty()) return (String)value;
        }
        try {
            Object value = get(codec, "getCodecName");
            if (value instanceof String && !((String)value).trim().isEmpty()) return (String)value;
        } catch (UnsupportedOperationException unavailable) { /* Older framework: inspect its own description. */ }
        Matcher match = Pattern.compile("codecName[:=]([^,}\\]]+)").matcher(codec.toString());
        return match.find() ? match.group(1).trim() : "Codec " + number(codec, "getCodecType");
    }
    static String family(String name) {
        String normalized = name.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]", "");
        if (normalized.contains("aptxlossless")) return "lossless";
        if (normalized.contains("aptxadaptive")) return "adaptive";
        return "other";
    }
    static List<String> modes(String family) {
        return "adaptive".equals(family) ? Arrays.asList("hq", "ll")
            : "lossless".equals(family) ? Collections.singletonList("hq") : Collections.emptyList();
    }
    static SortedMap<Integer, Integer> numericFlags(Class<?> type, String prefix) throws Exception {
        SortedMap<Integer, Integer> out = new TreeMap<>();
        for (Field field : type.getFields()) {
            if (!Modifier.isStatic(field.getModifiers()) || field.getType() != int.class ||
                !field.getName().startsWith(prefix)) continue;
            String suffix = field.getName().substring(prefix.length());
            if (!suffix.matches("[0-9]+")) continue;
            int flag = field.getInt(null);
            if (flag != 0 && (flag & (flag - 1)) == 0) out.put(Integer.parseInt(suffix), flag);
        }
        return out;
    }
    static List<Integer> values(Object codec, String prefix, String getter) throws Exception {
        List<Integer> out = new ArrayList<>();
        int mask = number(codec, getter);
        for (Map.Entry<Integer, Integer> entry : numericFlags(codec.getClass(), prefix).entrySet())
            if ((mask & entry.getValue()) == entry.getValue()) out.add(entry.getKey());
        return out;
    }
    static int requireFlag(Object codec, String prefix, String getter, int value) throws Exception {
        Integer flag = numericFlags(codec.getClass(), prefix).get(value);
        if (flag == null || (number(codec, getter) & flag) != flag)
            throw new IllegalArgumentException("Peer does not advertise " + prefix + value);
        return flag;
    }
    static Map<String, Object> describe(Object codec, boolean capability) throws Exception {
        if (codec == null) return ControlJson.object("available", false);
        String name = name(codec), family = family(name);
        long specific1 = ((Number)get(codec, "getCodecSpecific1")).longValue();
        String mode = "lossless".equals(family) ? "hq" : !"adaptive".equals(family) ? "unknown"
            : specific1 == ADAPTIVE_HQ ? "hq" : specific1 == ADAPTIVE_LL ? "ll"
            : specific1 == 0 ? "default" : "unknown";
        Map<String, Object> out = ControlJson.object("available", true, "name", name, "family", family,
            "type", number(codec, "getCodecType"), "sample_rates_hz", values(codec, "SAMPLE_RATE_", "getSampleRate"),
            "bit_depths", values(codec, "BITS_PER_SAMPLE_", "getBitsPerSample"), "mode", mode,
            "codec_specific1", specific1);
        if (capability) out.put("modes", modes(family));
        return out;
    }
    static List<Object> selectable(Object status) throws Exception {
        Object value = get(status, "getCodecsSelectableCapabilities");
        List<Object> out = new ArrayList<>();
        if (value instanceof Iterable) for (Object item : (Iterable<?>)value) { if (item != null) out.add(item); }
        else if (value != null && value.getClass().isArray())
            for (int i = 0; i < Array.getLength(value); i++) { Object item = Array.get(value, i); if (item != null) out.add(item); }
        else throw new IllegalStateException("Android returned no selectable codec list");
        return out;
    }
    static final class Request {
        final String address, family, mode;
        final int rate, bits;
        Request(String address, String family, String mode, int rate, int bits) {
            this.address = address; this.family = family; this.mode = mode; this.rate = rate; this.bits = bits;
        }
        Map<String, Object> describe() {
            return ControlJson.object("family", family, "mode", mode, "sample_rate_hz", rate, "bit_depth", bits);
        }
    }
    static Request parse(String[] args) {
        if (args.length == 0 || "get".equals(args[0])) {
            if (args.length > 2) throw new IllegalArgumentException(USAGE);
            return new Request(args.length == 2 ? address(args[1]) : "active", null, null, 0, 0);
        }
        if (args.length != 6 || !"select".equals(args[0])) throw new IllegalArgumentException(USAGE);
        String family = args[2], mode = args[3];
        if (!modes(family).contains(mode)) throw new IllegalArgumentException("Unsupported codec or mode");
        if (!args[4].matches("[1-9][0-9]{3,5}") || !args[5].matches("16|24|32"))
            throw new IllegalArgumentException("Invalid sample rate or bit depth");
        int rate = Integer.parseInt(args[4]), bits = Integer.parseInt(args[5]);
        if ("lossless".equals(family) && (rate != 44100 || bits != 16))
            throw new IllegalArgumentException("Lossless selection requires 44100 Hz / 16 bit");
        return new Request(address(args[1]), family, mode, rate, bits);
    }
    static String address(String address) {
        if ("active".equals(address)) return address;
        String normalized = address.toUpperCase(Locale.ROOT);
        if (!normalized.matches("[0-9A-F]{2}(:[0-9A-F]{2}){5}")) throw new IllegalArgumentException("Invalid Bluetooth address");
        return normalized;
    }
    static boolean sameType(Object one, Object two) throws Exception {
        Object a = extendedType(one), b = extendedType(two);
        return a != null && b != null ? a.equals(b)
            : a == null && b == null && number(one, "getCodecType") == number(two, "getCodecType");
    }
    static Object choose(List<Object> capabilities, Request request) throws Exception {
        Object match = null;
        for (Object codec : capabilities) {
            if (!request.family.equals(family(name(codec)))) continue;
            if (!values(codec, "SAMPLE_RATE_", "getSampleRate").contains(request.rate) ||
                !values(codec, "BITS_PER_SAMPLE_", "getBitsPerSample").contains(request.bits)) continue;
            if (!modes(request.family).contains(request.mode)) continue;
            if (match != null && !sameType(match, codec))
                throw new IllegalArgumentException("Multiple selectable codec types match; Android must resolve the ambiguity");
            match = codec;
        }
        if (match == null) throw new IllegalArgumentException("Requested codec, rate and bit depth are not selectable for this peer");
        return match;
    }
    static Object build(Object capability, Request request) throws Exception {
        Class<?> type = capability.getClass();
        Object builder = Class.forName(type.getName() + "$Builder").getConstructor().newInstance();
        Object extended = extendedType(capability);
        if (extended != null) {
            // Preserve the actual extended type object; an integer rebuild loses vendor identities.
            Method setter = null;
            for (Method method : builder.getClass().getMethods())
                if (method.getName().equals("setExtendedCodecType") && method.getParameterTypes().length == 1 &&
                    method.getParameterTypes()[0].isInstance(extended)) { setter = method; break; }
            if (setter == null) throw new UnsupportedOperationException("Extended codec selection unavailable");
            call(builder, setter.getName(), setter.getParameterTypes(), extended);
        } else call(builder, "setCodecType", new Class<?>[] {int.class}, number(capability, "getCodecType"));
        call(builder, "setCodecPriority", new Class<?>[] {int.class}, type.getField("CODEC_PRIORITY_HIGHEST").getInt(null));
        call(builder, "setSampleRate", new Class<?>[] {int.class}, requireFlag(capability, "SAMPLE_RATE_", "getSampleRate", request.rate));
        call(builder, "setBitsPerSample", new Class<?>[] {int.class}, requireFlag(capability, "BITS_PER_SAMPLE_", "getBitsPerSample", request.bits));
        int stereo = type.getField("CHANNEL_MODE_STEREO").getInt(null);
        if ((number(capability, "getChannelMode") & stereo) != stereo)
            throw new IllegalArgumentException("Peer does not advertise stereo for this codec");
        call(builder, "setChannelMode", new Class<?>[] {int.class}, stereo);
        // The dedicated Lossless type owns its selection semantics. Do not copy another codec's opaque words.
        long mode = "adaptive".equals(request.family) ? ("hq".equals(request.mode) ? ADAPTIVE_HQ : ADAPTIVE_LL) : 0;
        call(builder, "setCodecSpecific1", new Class<?>[] {long.class}, mode);
        return get(builder, "build");
    }
    static boolean matches(Object observed, Object selected, Request request) throws Exception {
        if (observed == null || !sameType(observed, selected)) return false;
        if (!values(observed, "SAMPLE_RATE_", "getSampleRate").equals(Collections.singletonList(request.rate)) ||
            !values(observed, "BITS_PER_SAMPLE_", "getBitsPerSample").equals(Collections.singletonList(request.bits))) return false;
        return !"adaptive".equals(request.family) || ((Number)get(observed, "getCodecSpecific1")).longValue() ==
            ("hq".equals(request.mode) ? ADAPTIVE_HQ : ADAPTIVE_LL);
    }
}
