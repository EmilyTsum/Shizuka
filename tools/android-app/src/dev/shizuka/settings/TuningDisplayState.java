// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/** Immutable projection of visible tuning values, excluding polling clocks and raw details. */
final class TuningDisplayState {
    static final class Input {
        boolean ready, working, capturing, hqControl, llControl, runtimeFresh, runtimeApplied;
        int hqMs, llMs;
        String error, errorKind, notice, operationId, operationError;
        LatencyApplyState.View operation;
        Map<String, ?> runtime = Collections.emptyMap();
    }
    static List<Object> key(Input i) {
        List<Object> values = new ArrayList<>();
        Collections.addAll(values, i.ready, i.error, i.errorKind);
        if (!i.ready || i.error != null) return Collections.unmodifiableList(values);
        Collections.addAll(values, i.notice, i.working, i.notice == null ? false : i.capturing,
            i.hqMs, i.llMs, i.hqControl, i.llControl, i.operationId, i.operationError,
            i.operation.title, i.operation.detail, i.operation.canApply, i.runtimeFresh, i.runtimeApplied);
        if (i.runtimeFresh) for (String name : new String[] {"rd_frames", "wr_frames", "owned_mappings",
                "in_flight_buffers", "confirmed_rewritten_wrappers", "graph_epoch", "stage", "error"})
            values.add(text(i.runtime, name));
        if (i.runtimeApplied) for (String name : new String[] {"active_delta_us", "last_before_ttp_us",
                "last_after_ttp_us", "profile", "sample_rate_hz"}) values.add(text(i.runtime, name));
        return Collections.unmodifiableList(values);
    }
    private static String text(Map<String, ?> values, String name) {
        Object value = values.get(name);
        return value == null ? "0" : value.toString();
    }
}
