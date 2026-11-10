// SPDX-License-Identifier: GPL-2.0-only
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.Objects;

/** Exact local codec-object preservation. No Android services or lifecycle writes. */
final class CodecSnapshot {
    private final Class<?> codecClass;
    private final Object extendedType;
    private final boolean extendedApi;
    private final int type, priority, rate, bits, channels;
    private final long[] specific;

    private CodecSnapshot(Object codec) throws Exception {
        Objects.requireNonNull(codec, "Missing observed codec");
        codecClass = codec.getClass();
        type = integer(codec, "getCodecType");
        priority = integer(codec, "getCodecPriority");
        rate = selectedFlag(codec, "getSampleRate");
        bits = selectedFlag(codec, "getBitsPerSample");
        channels = selectedFlag(codec, "getChannelMode");
        Method identity;
        try { identity = codecClass.getMethod("getExtendedCodecType"); }
        catch (NoSuchMethodException olderApi) { identity = null; }
        extendedApi = identity != null;
        extendedType = identity == null ? null : invoke(identity, codec);
        if (extendedApi && extendedType == null)
            throw new IllegalArgumentException("Extended codec identity is unavailable");
        specific = new long[4];
        for (int i = 0; i < specific.length; i++) {
            Object value = get(codec, "getCodecSpecific" + (i + 1));
            if (!(value instanceof Long)) throw new IllegalArgumentException("Invalid codec-specific word type");
            specific[i] = (Long)value;
        }
    }
    static CodecSnapshot capture(Object observed) throws Exception { return new CodecSnapshot(observed); }
    private static Object invoke(Method method, Object object, Object... args) throws Exception {
        try { return method.invoke(object, args); }
        catch (InvocationTargetException wrapped) {
            Throwable cause = wrapped.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            if (cause instanceof Error) throw (Error)cause;
            throw new IllegalStateException("Codec method failed", cause);
        }
    }
    private static Object get(Object object, String method) throws Exception {
        return invoke(object.getClass().getMethod(method), object);
    }
    private static int integer(Object object, String method) throws Exception {
        Object value = get(object, method);
        if (!(value instanceof Integer)) throw new IllegalArgumentException("Invalid codec integer: " + method);
        return (Integer)value;
    }
    private static int selectedFlag(Object object, String method) throws Exception {
        int flag = integer(object, method);
        if (flag <= 0 || (flag & (flag - 1)) != 0)
            throw new IllegalArgumentException("Observed codec must have one selected flag: " + method);
        return flag;
    }
    private boolean sameType(Object codec) throws Exception {
        if (integer(codec, "getCodecType") != type) return false;
        try {
            Object current = get(codec, "getExtendedCodecType");
            return extendedApi && extendedType.equals(current);
        } catch (NoSuchMethodException olderApi) { return !extendedApi; }
    }
    /** A gate for an already selected tuple, not a claim of relay/profile support. */
    boolean advertisedBy(Object capability) throws Exception {
        if (capability == null || !sameType(capability)) return false;
        return (integer(capability, "getSampleRate") & rate) == rate &&
            (integer(capability, "getBitsPerSample") & bits) == bits &&
            (integer(capability, "getChannelMode") & channels) == channels;
    }
    boolean matches(Object observed) throws Exception {
        CodecSnapshot other = capture(observed);
        return codecClass == other.codecClass && type == other.type && priority == other.priority && rate == other.rate && bits == other.bits &&
            channels == other.channels && extendedApi == other.extendedApi &&
            Objects.equals(extendedType, other.extendedType) && Arrays.equals(specific, other.specific);
    }
    private static void set(Object builder, String method, Class<?> type, Object value) throws Exception {
        invoke(builder.getClass().getMethod(method, type), builder, value);
    }
    /** Build a local clone and verify every field before a caller could submit it. */
    Object cloneObject() throws Exception {
        Object builder = Class.forName(codecClass.getName() + "$Builder", true, codecClass.getClassLoader())
            .getConstructor().newInstance();
        if (extendedApi) {
            Method setter = null;
            for (Method candidate : builder.getClass().getMethods())
                if (candidate.getName().equals("setExtendedCodecType") && candidate.getParameterTypes().length == 1 &&
                        candidate.getParameterTypes()[0].isInstance(extendedType)) {
                    if (setter != null) throw new IllegalArgumentException("Ambiguous extended codec setter");
                    setter = candidate;
                }
            if (setter == null) throw new NoSuchMethodException("Extended codec setter unavailable");
            invoke(setter, builder, extendedType);
        } else set(builder, "setCodecType", int.class, type);
        set(builder, "setCodecPriority", int.class, priority);
        set(builder, "setSampleRate", int.class, rate);
        set(builder, "setBitsPerSample", int.class, bits);
        set(builder, "setChannelMode", int.class, channels);
        for (int i = 0; i < specific.length; i++) set(builder, "setCodecSpecific" + (i + 1), long.class, specific[i]);
        Object restored = get(builder, "build");
        if (restored == null || restored.getClass() != codecClass || !matches(restored))
            throw new IllegalStateException("Codec builder did not preserve the observed configuration");
        return restored;
    }
}
