// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class TuningDisplayStateTest {
    static int checks;
    static void check(boolean ok) { checks++; if (!ok) throw new AssertionError("tuning check " + checks); }
    static TuningDisplayState.Input input() {
        TuningDisplayState.Input i = new TuningDisplayState.Input();
        i.ready = i.hqControl = i.llControl = true; i.hqMs = 150;
        i.operation = new LatencyApplyState.View(LatencyApplyState.Phase.SAVED, "Saved request", "Pending", false, true, 0);
        return i;
    }
    public static void main(String[] args) {
        TuningDisplayState.Input i = input();
        Map<String, Object> runtime = new HashMap<>(); i.runtime = runtime;
        runtime.put("rd_frames", 3); runtime.put("wr_frames", 2);
        List<Object> initial = TuningDisplayState.key(i);
        for (String key : new String[] {"observation_age_ms", "lease_remaining_ms", "monotonic_now_ns", "generation", "reason"}) {
            runtime.put(key, 999L); check(initial.equals(TuningDisplayState.key(i)));
        }
        runtime.put("wr_frames", 999); check(initial.equals(TuningDisplayState.key(i))); // Hidden stale counters.
        i.runtimeFresh = true;
        List<Object> fresh = TuningDisplayState.key(i); check(!initial.equals(fresh));
        runtime.put("observation_age_ms", 1000); check(fresh.equals(TuningDisplayState.key(i)));
        runtime.put("wr_frames", 1000); check(!fresh.equals(TuningDisplayState.key(i)));
        fresh = TuningDisplayState.key(i);
        runtime.put("stage", 4); check(!fresh.equals(TuningDisplayState.key(i)));
        fresh = TuningDisplayState.key(i);
        runtime.put("profile", 1); check(fresh.equals(TuningDisplayState.key(i))); // Not displayed until applied.
        i.runtimeApplied = true; List<Object> applied = TuningDisplayState.key(i); check(!fresh.equals(applied));
        for (String key : new String[] {"active_delta_us", "last_before_ttp_us", "last_after_ttp_us", "profile", "sample_rate_hz"}) {
            List<Object> before = TuningDisplayState.key(i); runtime.put(key, 777); check(!before.equals(TuningDisplayState.key(i)));
        }
        i = input(); initial = TuningDisplayState.key(i);
        i.hqMs = 50; check(!initial.equals(TuningDisplayState.key(i))); i.hqMs = 150;
        i.llMs = 50; check(!initial.equals(TuningDisplayState.key(i))); i.llMs = 0;
        i.working = true; check(!initial.equals(TuningDisplayState.key(i))); i.working = false;
        i.hqControl = false; check(!initial.equals(TuningDisplayState.key(i))); i.hqControl = true;
        i.llControl = false; check(!initial.equals(TuningDisplayState.key(i))); i.llControl = true;
        i.notice = "Saved"; check(!initial.equals(TuningDisplayState.key(i))); i.notice = null;
        i.operationId = LatencyApplyStateTest.ID; check(!initial.equals(TuningDisplayState.key(i))); i.operationId = null;
        i.operationError = "Session ended"; check(!initial.equals(TuningDisplayState.key(i))); i.operationError = null;
        i.error = "Unavailable"; check(!initial.equals(TuningDisplayState.key(i)));
        List<Object> unavailable = TuningDisplayState.key(i);
        i.hqMs = 12; i.working = true; check(unavailable.equals(TuningDisplayState.key(i))); // Blocking screen hides these.
        i.errorKind = "session"; check(!unavailable.equals(TuningDisplayState.key(i)));
        i = input(); i.ready = false; List<Object> opening = TuningDisplayState.key(i);
        i.operationId = LatencyApplyStateTest.ID; check(opening.equals(TuningDisplayState.key(i)));
        i.ready = true; check(!opening.equals(TuningDisplayState.key(i)));

        LatencyApplyState.Expected expected = LatencyApplyStateTest.expected();
        LatencyApplyState.Status status = LatencyApplyStateTest.applied();
        i = input(); i.operationId = LatencyApplyStateTest.ID;
        i.operation = LatencyApplyState.evaluate(expected, status, 1000, true, false);
        List<Object> proof = TuningDisplayState.key(i);
        status.receivedElapsedMs = 2000;
        i.operation = LatencyApplyState.evaluate(expected, status, 2000, true, false);
        check(proof.equals(TuningDisplayState.key(i))); // A fresh identical reply does not rebuild views.
        i.operation = LatencyApplyState.evaluate(expected, status, 3000, true, false);
        check(!proof.equals(TuningDisplayState.key(i))); // Independent expiry still changes visible state.
        status = LatencyApplyStateTest.status(); status.operationId = null; status.currentOperationId = null;
        status.operationKnown = false; status.phase = "saved_pending"; status.terminal = true;
        i.operationId = null;
        LatencyApplyState.Expected saved = new LatencyApplyState.Expected(null, 150, 0, 3);
        i.operation = LatencyApplyState.evaluate(saved, status, 1000, true, false);
        check(i.operation.canApply); List<Object> qualified = TuningDisplayState.key(i);
        i.operation = LatencyApplyState.evaluate(saved, status, 3000, true, false);
        check(!i.operation.canApply && !qualified.equals(TuningDisplayState.key(i)));
        System.out.println("Tuning display projection: " + checks + " checks passed.");
    }
}
