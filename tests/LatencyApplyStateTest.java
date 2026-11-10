// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.util.HashMap;
import java.util.Map;

public final class LatencyApplyStateTest {
    static final String ID = "01234567-89ab-cdef-0123-456789abcdef";
    static int checks;
    static void check(boolean value) { checks++; if (!value) throw new AssertionError("check " + checks); }
    static LatencyApplyState.Status status() {
        LatencyApplyState.Status s = new LatencyApplyState.Status();
        s.schema = 1; s.scope = "latency_application"; s.operationId = ID;
        s.operationKnownReported = s.operationKnown = s.currentOperationReported = true;
        s.currentOperationId = ID; s.currentBootId = "11111111-2222-3333-4444-555555555555";
        s.available = s.currentContextSupported = s.currentConfigMatches = s.retrySafe = true;
        s.requestedHqMs = 150; s.requestedLlMs = 0; s.rootGeneration = 3;
        s.receivedElapsedMs = 1000; s.phase = "queued";
        return s;
    }
    static LatencyApplyState.Status applied() {
        LatencyApplyState.Status s = status();
        s.phase = "applied"; s.terminal = s.applied = s.fresh = s.leaseValid = s.newEpoch = true;
        s.graphEpoch = "7"; s.activeDeltaUs = 150000;
        s.wrFrames = 3; s.confirmedRewrittenWrappers = 6;
        s.observationAgeMs = 100; s.leaseRemainingMs = 1000;
        return s;
    }
    static LatencyApplyState.Expected expected() { return new LatencyApplyState.Expected(ID, 150, 0, 3); }
    static LatencyApplyState.Expected zero() { return new LatencyApplyState.Expected(ID, 0, 0, 3); }
    static LatencyApplyState.Status disabled() {
        LatencyApplyState.Status s = status();
        s.phase = "disabled"; s.requestedHqMs = 0;
        s.available = s.currentContextSupported = false;
        s.disableAvailable = s.terminal = s.disabled = s.disableReceiptValid = s.fresh = true;
        s.newSession = s.normalOwnerVerified = s.oldRelayRemoved = s.stockGslVerified = true;
        s.observationAgeMs = 100;
        return s;
    }
    static LatencyApplyState.View zeroView(LatencyApplyState.Status s, long now) {
        return LatencyApplyState.evaluate(zero(), s, now, true, false);
    }
    static LatencyApplyState.View view(LatencyApplyState.Status s) {
        return LatencyApplyState.evaluate(expected(), s, 1000, true, false);
    }
    static void unconfirmed(LatencyApplyState.Status s) { check(view(s).phase != LatencyApplyState.Phase.APPLIED); }

    static void testStatusTypesAndReboundRequests() {
        Map<String, Object> values = new HashMap<>();
        values.put("schema", 1); values.put("scope", "latency_application");
        values.put("phase", "saved_pending"); values.put("operation_id", null);
        values.put("available", true); values.put("current_context_supported", true);
        values.put("terminal", true); values.put("retry_safe", true);
        values.put("operation_known", false); values.put("current_operation_id", null);
        values.put("current_boot_id", "11111111-2222-3333-4444-555555555555");
        LatencyApplyState.Expected saved = new LatencyApplyState.Expected(null, 150, 0, 3);
        LatencyApplyState.Status s = LatencyApplyState.decode(values, 3, 1000);
        check(LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);
        check(LatencyApplyState.evaluate(saved, s, 1000, true, false).phase == LatencyApplyState.Phase.SAVED);
        check(!LatencyApplyState.evaluate(saved, s, 3000, true, false).canApply);
        values.remove("retry_safe");
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("retry_safe", "true");
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("retry_safe", true); values.put("available", "true");
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("available", true); values.put("phase", "queued");
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("phase", "saved_pending"); values.put("operation_id", "not-an-id");
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("operation_id", null); values.put("schema", 4294967297L);
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("schema", 1); values.put("operation_id", 7);
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.remove("operation_id");
        check(!LatencyApplyState.evaluate(saved, LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);
        values.put("operation_id", ID); values.put("requested_hq_ms", "150");
        check(LatencyApplyState.decode(values, 3, 1000).requestedHqMs == -1);
        values.put("requested_hq_ms", 150.0);
        check(LatencyApplyState.decode(values, 3, 1000).requestedHqMs == -1);
        values.put("requested_hq_ms", Long.MAX_VALUE);
        check(LatencyApplyState.decode(values, 3, 1000).requestedHqMs == -1);
        values.put("requested_hq_ms", 150); values.put("requested_ll_ms", 50);
        values.put("active_delta_us", 150000L); values.put("fresh", "true"); values.put("graph_epoch", 7);
        s = LatencyApplyState.decode(values, 3, 1000);
        check(s.requestedHqMs == 150 && s.requestedLlMs == 50 && s.activeDeltaUs == 150000);
        check(!s.fresh && s.graphEpoch.isEmpty());
        values.put("phase", "unconfirmed"); values.put("retry_safe", false);
        values.put("accepted", true); values.put("last_outcome", "applied");
        check(!LatencyApplyState.evaluate(expected(), LatencyApplyState.decode(values, 3, 1000), 1000, true, false).canApply);

        values.put("operation_known", false); values.put("current_operation_id", null);
        values.put("current_boot_id", "11111111-2222-3333-4444-555555555555");
        values.put("operation_id", ID); values.put("phase", "unconfirmed");
        values.put("retry_safe", true);
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000)); // Unknown-ID reply never clears.
        values.put("operation_id", null); values.put("phase", "saved_pending");
        s = LatencyApplyState.decode(values, 3, 1000);
        check(LatencyApplyState.reconcileLatest(s, 3, 1000));
        check(!LatencyApplyState.reconcileLatest(s, 4, 1000));
        check(!LatencyApplyState.reconcileLatest(s, 3, 999));
        check(!LatencyApplyState.reconcileLatest(s, 3, 3000));
        values.put("retry_safe", false);
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));
        values.put("retry_safe", true); values.remove("operation_known");
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));
        values.put("operation_known", "false");
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));
        values.put("operation_known", false); values.remove("current_operation_id");
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));
        values.put("current_operation_id", null); values.put("current_boot_id", "unknown");
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));
        values.put("current_boot_id", "11111111-2222-3333-4444-555555555555");
        values.put("operation_known", true); values.put("operation_id", ID);
        values.put("current_operation_id", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));
        values.put("current_operation_id", ID); values.put("phase", "queued"); values.put("retry_safe", false);
        check(LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000)); // Adopt current work, never launch it.
        values.put("current_operation_id", 7);
        check(!LatencyApplyState.reconcileLatest(LatencyApplyState.decode(values, 3, 1000), 3, 1000));

        LatencyApplyState.Requests gate = new LatencyApplyState.Requests();
        LatencyApplyState.Requests.Ticket old = gate.begin(3);
        check(old != null && gate.begin(3) == null); // A slow status call cannot queue repeat polls.
        check(gate.current(old, 3) && !gate.current(old, 4));
        gate.reset();
        LatencyApplyState.Requests.Ticket rebound = gate.begin(4);
        check(rebound != null && rebound.serial != old.serial);
        check(!gate.finish(old, 4)); // Old completion cannot clear the new session's in-flight call.
        check(gate.current(rebound, 4) && gate.begin(4) == null);
        check(gate.finish(rebound, 4) && !gate.finish(rebound, 4));
        LatencyApplyState.Requests.Ticket next = gate.begin(4);
        check(next != null && !gate.current(rebound, 4));
        gate.reset(); check(!gate.current(next, 4));
    }

    public static void main(String[] args) {
        LatencyApplyState.Expected saved = new LatencyApplyState.Expected(null, 150, 0, 3);
        LatencyApplyState.Status s = status(); s.operationId = null; s.currentOperationId = null;
        s.operationKnown = false; s.phase = "unavailable";
        s.available = false; s.currentContextSupported = false;
        LatencyApplyState.View v = LatencyApplyState.evaluate(saved, s, 1000, true, false);
        check(v.phase == LatencyApplyState.Phase.SAVED && !v.canApply);
        s.available = s.currentContextSupported = true; s.phase = "saved_pending"; s.terminal = true;
        check(LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);
        s.currentContextSupported = false;
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply); // Never force HQ44 for HQ96.
        s.currentContextSupported = true;
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, true).canApply);
        check(!LatencyApplyState.evaluate(saved, s, 1000, false, false).canApply);
        s.phase = "unknown_future_phase";
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);
        s.phase = "unconfirmed"; s.retrySafe = false;
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);
        s.lastOutcome = "applied"; s.retrySafe = true;
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);
        s.phase = "saved_pending";
        check(LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);
        check(!LatencyApplyState.evaluate(saved, s, 3000, true, false).canApply);
        s.phase = null;
        check(LatencyApplyState.evaluate(saved, s, 1000, true, false).phase == LatencyApplyState.Phase.UNCONFIRMED);
        check(LatencyApplyState.evaluate(expected(), null, 1000, true, false).poll); // Lost launch response: same UUID poll.
        check(!LatencyApplyState.evaluate(expected(), null, 1000, true, false).canApply);
        s = status(); check(LatencyApplyState.evaluate(saved, s, 1000, true, false).phase == LatencyApplyState.Phase.PENDING);
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply);

        String[] phases = {"queued", "preflight", "reconfiguring", "restoring", "verifying", "waiting_for_playback"};
        LatencyApplyState.Phase[] shown = {LatencyApplyState.Phase.PENDING, LatencyApplyState.Phase.PENDING,
            LatencyApplyState.Phase.RECONFIGURING, LatencyApplyState.Phase.RECONFIGURING,
            LatencyApplyState.Phase.VERIFYING, LatencyApplyState.Phase.WAITING_FOR_PLAYBACK};
        for (int i = 0; i < phases.length; i++) {
            s = status(); s.phase = phases[i]; v = view(s);
            check(v.phase == shown[i] && v.poll && !v.canApply);
        }
        s = status(); s.phase = "waiting_for_playback"; s.terminal = true;
        check(view(s).phase == LatencyApplyState.Phase.WAITING_FOR_PLAYBACK && !view(s).canApply);

        s = applied(); v = view(s);
        check(v.phase == LatencyApplyState.Phase.APPLIED && v.poll && !v.canApply);
        check(v.title.equals("Applied HQ +150 ms") && v.proofExpiryDelayMs == 1001);
        check(v.detail.contains("R3 HQ at 44.1 kHz"));
        check(LatencyApplyState.evaluate(expected(), s, 1999, true, false).phase == LatencyApplyState.Phase.APPLIED);
        check(LatencyApplyState.evaluate(expected(), s, 2000, true, false).phase == LatencyApplyState.Phase.UNCONFIRMED);
        check(LatencyApplyState.evaluate(expected(), s, 999, true, false).phase != LatencyApplyState.Phase.APPLIED);
        s.observationAgeMs = 1999; s.leaseRemainingMs = 2000;
        check(view(s).proofExpiryDelayMs == 2);
        check(LatencyApplyState.evaluate(expected(), s, 1001, true, false).phase != LatencyApplyState.Phase.APPLIED);
        s = applied(); s.lastOutcome = "applied"; s.applied = false; unconfirmed(s);
        s = applied(); s.phase = "verifying"; check(view(s).phase == LatencyApplyState.Phase.VERIFYING);
        s = applied(); s.terminal = false; unconfirmed(s);
        s = applied(); s.available = false; unconfirmed(s);
        s = applied(); s.disabled = true; unconfirmed(s);
        s = applied(); s.currentContextSupported = false; unconfirmed(s);
        s = applied(); s.operationKnown = false; unconfirmed(s);
        s = applied(); s.operationKnownReported = false; unconfirmed(s);
        s = applied(); s.currentOperationReported = false; unconfirmed(s);
        s = applied(); s.currentOperationId = null; unconfirmed(s);
        s = applied(); s.currentOperationId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"; unconfirmed(s);
        s.phase = "superseded"; check(!view(s).canApply); // An old terminal record is not the current owner.
        s = applied(); s.currentBootId = "unknown"; unconfirmed(s);
        s = applied(); s.fresh = false; unconfirmed(s);
        s = applied(); s.leaseValid = false; unconfirmed(s);
        s = applied(); s.newEpoch = false; unconfirmed(s);
        s = applied(); s.pendingHq = true; unconfirmed(s);
        s = applied(); s.wrFrames = 0; unconfirmed(s);
        s = applied(); s.confirmedRewrittenWrappers = 0; unconfirmed(s);
        s = applied(); s.activeDeltaUs = 100000; unconfirmed(s);
        for (long age : new long[]{-1, 2000, Long.MAX_VALUE}) { s = applied(); s.observationAgeMs = age; unconfirmed(s); }
        for (long left : new long[]{0, -1, 2001, Long.MAX_VALUE}) { s = applied(); s.leaseRemainingMs = left; unconfirmed(s); }
        for (String epoch : new String[]{"0", "07", "-1", "", "18446744073709551616"}) { s = applied(); s.graphEpoch = epoch; unconfirmed(s); }
        s = applied(); s.graphEpoch = "18446744073709551615"; check(view(s).phase == LatencyApplyState.Phase.APPLIED);
        s = applied(); s.schema = 2; unconfirmed(s);
        s = applied(); s.scope = "another_api"; unconfirmed(s);
        s = applied(); s.rootGeneration = 4; unconfirmed(s);
        s = applied(); s.operationId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"; unconfirmed(s);
        s = applied(); s.operationId = null; unconfirmed(s);
        s = applied(); s.receivedElapsedMs = 0; unconfirmed(s);
        s = applied(); s.receivedElapsedMs = Long.MAX_VALUE; unconfirmed(s);
        s = applied(); s.phase = "future_unknown_state"; unconfirmed(s); check(!view(s).canApply);

        s = applied(); s.requestedLlMs = 50; s.pendingLl = true;
        v = LatencyApplyState.evaluate(new LatencyApplyState.Expected(ID, 150, 50, 3), s, 1000, true, false);
        check(v.phase == LatencyApplyState.Phase.APPLIED && v.title.contains("HQ"));
        check(v.detail.contains("LL remains pending"));
        s = applied(); s.requestedHqMs = 0; s.activeDeltaUs = 0;
        v = zeroView(s, 1000);
        check(v.phase == LatencyApplyState.Phase.UNCONFIRMED); // No invented zero-removal proof.
        s.confirmedRewrittenWrappers = 0; check(zeroView(s, 1000).phase == LatencyApplyState.Phase.UNCONFIRMED);
        s = disabled(); v = zeroView(s, 1000);
        check(v.phase == LatencyApplyState.Phase.DISABLED && !v.canApply && v.poll);
        check(v.title.equals("Additional HQ delay disabled") && v.proofExpiryDelayMs == 1901);
        check(!s.leaseValid && s.leaseRemainingMs == 0 && !s.currentContextSupported && s.wrFrames == 0);
        s.disableAvailable = false; check(zeroView(s, 1000).phase == LatencyApplyState.Phase.UNCONFIRMED);
        s.disableAvailable = true;
        check(zeroView(s, 2899).phase == LatencyApplyState.Phase.DISABLED);
        check(zeroView(s, 2900).phase == LatencyApplyState.Phase.UNCONFIRMED);
        check(zeroView(s, 999).phase == LatencyApplyState.Phase.UNCONFIRMED);
        for (int missing = 0; missing < 16; missing++) {
            s = disabled();
            switch (missing) {
                case 0: s.disabled = false; break;
                case 1: s.disableReceiptValid = false; break;
                case 2: s.fresh = false; break;
                case 3: s.newSession = false; break;
                case 4: s.normalOwnerVerified = false; break;
                case 5: s.oldRelayRemoved = false; break;
                case 6: s.stockGslVerified = false; break;
                case 7: s.pendingHq = true; break;
                case 8: s.applied = true; break;
                case 9: s.terminal = false; break;
                case 10: s.observationAgeMs = 2000; break;
                case 11: s.operationKnown = false; break;
                case 12: s.operationKnownReported = false; break;
                case 13: s.currentOperationReported = false; break;
                case 14: s.currentOperationId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"; break;
                case 15: s.currentBootId = "unknown"; break;
                default: throw new AssertionError();
            }
            check(zeroView(s, 1000).phase != LatencyApplyState.Phase.DISABLED);
        }
        s = disabled(); s.currentConfigMatches = false; check(zeroView(s, 1000).phase == LatencyApplyState.Phase.SUPERSEDED);
        s = disabled(); s.operationId = null; check(zeroView(s, 1000).phase == LatencyApplyState.Phase.UNCONFIRMED);
        s = disabled(); s.rootGeneration = 4; check(zeroView(s, 1000).phase == LatencyApplyState.Phase.UNCONFIRMED);
        s = disabled(); s.requestedLlMs = 50; s.pendingLl = true;
        v = LatencyApplyState.evaluate(new LatencyApplyState.Expected(ID, 0, 50, 3), s, 1000, true, false);
        check(v.phase == LatencyApplyState.Phase.DISABLED && v.detail.contains("LL remains pending"));
        check(view(s).phase != LatencyApplyState.Phase.DISABLED);
        s = disabled(); s.phase = "saved_pending"; s.operationId = null; s.currentOperationId = null; s.operationKnown = false;
        LatencyApplyState.Expected savedZero = new LatencyApplyState.Expected(null, 0, 0, 3);
        check(LatencyApplyState.evaluate(savedZero, s, 1000, true, false).canApply); // Owner removal can preserve HQ96.
        s.disableAvailable = false;
        check(!LatencyApplyState.evaluate(savedZero, s, 1000, true, false).canApply);
        s.disableAvailable = true;
        check(!LatencyApplyState.evaluate(saved, s, 1000, true, false).canApply); // Removal support is not positive support.
        s = applied(); s.currentConfigMatches = false; check(view(s).phase == LatencyApplyState.Phase.SUPERSEDED);
        s = applied(); s.terminal = false; s.requestedHqMs = 100;
        check(view(s).phase == LatencyApplyState.Phase.SUPERSEDED && !view(s).canApply);
        s.terminal = true; check(view(s).canApply);
        s = applied(); s.phase = "failed"; check(view(s).phase == LatencyApplyState.Phase.FAILED && !view(s).canApply);
        s = applied(); s.phase = "unconfirmed"; s.retrySafe = false;
        check(view(s).phase == LatencyApplyState.Phase.UNCONFIRMED && !view(s).canApply);
        s.retrySafe = true; s.lastOutcome = "applied";
        check(view(s).phase == LatencyApplyState.Phase.UNCONFIRMED && view(s).canApply);
        s = status(); s.phase = "rejected"; s.terminal = true;
        check(view(s).phase == LatencyApplyState.Phase.REJECTED);
        s.phase = "recovered"; check(view(s).phase == LatencyApplyState.Phase.RECOVERED);
        s.phase = "superseded"; check(view(s).phase == LatencyApplyState.Phase.SUPERSEDED);
        boolean rejected = false;
        try { new LatencyApplyState.Expected("not-an-operation", 150, 0, 3); } catch (IllegalArgumentException e) { rejected = true; }
        check(rejected);
        rejected = false;
        try { new LatencyApplyState.Expected(ID, 151, 0, 3); } catch (IllegalArgumentException e) { rejected = true; }
        check(rejected);
        s = status(); s.operationId = null; s.currentOperationId = null; s.operationKnown = false;
        s.phase = "saved_pending"; s.terminal = true; s.available = false;
        s.reason = "application helper unavailable; existing ownership not reconciled";
        v = LatencyApplyState.evaluate(saved, s, 1000, true, false);
        check(!v.canApply && !v.detail.contains("helper") && !v.detail.contains("ownership"));
        check(s.reason.equals("application helper unavailable; existing ownership not reconciled")); // Exact details retained.
        testStatusTypesAndReboundRequests();
        System.out.println("Delay operation UI: " + checks + " checks passed.");
    }
}
