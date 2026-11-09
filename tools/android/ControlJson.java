// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Portable JSON output for internal control models and command-line consumers. */
final class ControlJson {
    static Map<String, Object> object(Object... pairs) {
        if (pairs.length % 2 != 0) throw new IllegalArgumentException("key/value pairs required");
        Map<String, Object> out = new LinkedHashMap<>();
        for (int i = 0; i < pairs.length; i += 2) out.put((String)pairs[i], pairs[i + 1]);
        return out;
    }
    static String encode(Object value) {
        if (value == null) return "null";
        if (value instanceof Boolean) return value.toString();
        if (value instanceof Number) {
            double number = ((Number)value).doubleValue();
            if (!Double.isFinite(number)) throw new IllegalArgumentException("non-finite JSON number");
            return value.toString();
        }
        if (value instanceof Map) {
            List<String> entries = new ArrayList<>();
            for (Map.Entry<?, ?> item : ((Map<?, ?>)value).entrySet())
                entries.add(encode(item.getKey().toString()) + ":" + encode(item.getValue()));
            return "{" + String.join(",", entries) + "}";
        }
        if (value instanceof Iterable) {
            List<String> items = new ArrayList<>();
            for (Object item : (Iterable<?>)value) items.add(encode(item));
            return "[" + String.join(",", items) + "]";
        }
        if (!(value instanceof String)) throw new IllegalArgumentException("unsupported JSON type");
        StringBuilder out = new StringBuilder("\"");
        for (char c : ((String)value).toCharArray()) {
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 32) out.append(String.format(Locale.ROOT, "\\u%04x", (int)c));
            else out.append(c);
        }
        return out.append('"').toString();
    }
}
