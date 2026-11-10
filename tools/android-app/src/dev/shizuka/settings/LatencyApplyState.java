// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.util.Map;

/** Pure presentation policy for the module's asynchronous delay operation.
 * It never starts work, changes a codec, presses Play, or retains a response.
 */
final class LatencyApplyState {
    static final String BUTTON = "Apply saved delay";
    static final long POLL_MS = 1000;
    private static final long PROOF_MS = 2000;

    enum Phase {
        SAVED, PENDING, RECONFIGURING, VERIFYING, WAITING_FOR_PLAYBACK,
        APPLIED, DISABLED, UNCONFIRMED, REJECTED, SUPERSEDED, RECOVERED, FAILED, UNAVAILABLE
    }

    static final class Expected {
        final String operationId;
        final int hqMs, llMs;
        final long rootGeneration;
        Expected(String operationId, int hqMs, int llMs, long rootGeneration) {
            if ((operationId != null && !uuid(operationId)) || hqMs < 0 || hqMs > 150 ||
                llMs < 0 || llMs > 100 || rootGeneration < 0)
                throw new IllegalArgumentException("Invalid delay operation identity or values");
            this.operationId = operationId; this.hqMs = hqMs; this.llMs = llMs;
            this.rootGeneration = rootGeneration;
        }
    }

    /** Populate once from a v1 latency_application response, then hand to the UI.
     * Receipt time and generation are local, never taken from JSON wall time.
     * Missing booleans stay false and missing proof numbers stay invalid.
     */
    static final class Status {
        int schema;
        String scope = "", operationId, currentOperationId, currentBootId = "", phase = "", lastOutcome = "", reason = "";
        boolean operationKnown, operationKnownReported, currentOperationReported;
        boolean available, disableAvailable, currentContextSupported, terminal, applied, currentConfigMatches, retrySafe;
        boolean pendingHq, pendingLl, fresh, leaseValid, newEpoch;
        boolean disabled, disableReceiptValid, newSession, normalOwnerVerified, oldRelayRemoved, stockGslVerified;
        int requestedHqMs = -1, requestedLlMs = -1;
        long rootGeneration = -1, receivedElapsedMs;
        long observationAgeMs = -1, leaseRemainingMs;
        long activeDeltaUs = -1, wrFrames, confirmedRewrittenWrappers;
        String graphEpoch = "0";
    }

    /** Each asynchronous response belongs to one request and one root binding. */
    static final class Requests {
        static final class Ticket {
            final long serial, generation;
            Ticket(long serial, long generation) { this.serial = serial; this.generation = generation; }
        }
        private long serial;
        private Ticket active;
        synchronized Ticket begin(long generation) {
            if (active != null) return null;
            active = new Ticket(++serial, generation);
            return active;
        }
        synchronized boolean current(Ticket ticket, long generation) {
            return ticket != null && active == ticket && ticket.generation == generation;
        }
        synchronized boolean finish(Ticket ticket, long generation) {
            if (!current(ticket, generation)) return false;
            active = null;
            return true;
        }
        synchronized void reset() { active = null; serial++; }
    }

    /** Strict JSON-compatible types. Strings/nonnumeric JSON values cannot grant capability. */
    static Status decode(Map<String, ?> values, long generation, long receipt) {
        Status s = new Status();
        s.schema = number(values, "schema", 0) == 1 ? 1 : 0;
        s.scope = text(values, "scope"); s.phase = text(values, "phase");
        Object id = values.get("operation_id");
        s.operationId = id instanceof String ? (String) id : null;
        if (!values.containsKey("operation_id") || (id != null && !(id instanceof String))) s.schema = 0;
        s.operationKnownReported = values.get("operation_known") instanceof Boolean;
        s.operationKnown = flag(values, "operation_known");
        Object current = values.get("current_operation_id");
        s.currentOperationReported = values.containsKey("current_operation_id") &&
            (current == null || current instanceof String);
        s.currentOperationId = current instanceof String ? (String) current : null;
        s.currentBootId = text(values, "current_boot_id");
        s.lastOutcome = text(values, "last_outcome"); s.reason = text(values, "reason");
        s.available = flag(values, "available"); s.disableAvailable = flag(values, "disable_available");
        s.currentContextSupported = flag(values, "current_context_supported");
        s.terminal = flag(values, "terminal"); s.retrySafe = flag(values, "retry_safe");
        s.applied = flag(values, "applied"); s.currentConfigMatches = flag(values, "current_config_matches");
        s.pendingHq = flag(values, "pending_hq"); s.pendingLl = flag(values, "pending_ll");
        s.fresh = flag(values, "fresh"); s.leaseValid = flag(values, "lease_valid"); s.newEpoch = flag(values, "new_epoch");
        s.disabled = flag(values, "disabled"); s.disableReceiptValid = flag(values, "disable_receipt_valid");
        s.newSession = flag(values, "new_session"); s.normalOwnerVerified = flag(values, "normal_owner_verified");
        s.oldRelayRemoved = flag(values, "old_relay_removed"); s.stockGslVerified = flag(values, "stock_gsl_verified");
        long hq = number(values, "requested_hq_ms", -1), ll = number(values, "requested_ll_ms", -1);
        s.requestedHqMs = hq >= 0 && hq <= 150 ? (int) hq : -1;
        s.requestedLlMs = ll >= 0 && ll <= 100 ? (int) ll : -1;
        s.observationAgeMs = number(values, "observation_age_ms", -1);
        s.leaseRemainingMs = number(values, "lease_remaining_ms", 0);
        s.activeDeltaUs = number(values, "active_delta_us", -1);
        s.wrFrames = number(values, "wr_frames", 0);
        s.confirmedRewrittenWrappers = number(values, "confirmed_rewritten_wrappers", 0);
        s.graphEpoch = text(values, "graph_epoch");
        s.rootGeneration = generation; s.receivedElapsedMs = receipt;
        return s;
    }
    private static String text(Map<String, ?> values, String key) {
        Object value = values.get(key);
        return value instanceof String ? (String) value : "";
    }
    private static boolean flag(Map<String, ?> values, String key) { return Boolean.TRUE.equals(values.get(key)); }
    static long number(Map<String, ?> values, String key, long fallback) {
        Object value = values.get(key);
        return value instanceof Byte || value instanceof Short || value instanceof Integer || value instanceof Long
            ? ((Number) value).longValue() : fallback;
    }

    /** A separate latest query may reconcile a missing local journal, never the missing-ID reply itself. */
    static boolean reconcileLatest(Status s, long generation, long now) {
        if (s == null || s.schema != 1 || !"latency_application".equals(s.scope) ||
            s.rootGeneration != generation || s.receivedElapsedMs <= 0 || now < s.receivedElapsedMs ||
            now - s.receivedElapsedMs >= PROOF_MS || !s.operationKnownReported ||
            !s.currentOperationReported || !uuid(s.currentBootId)) return false;
        if (s.operationKnown)
            return uuid(s.operationId) && s.operationId.equals(s.currentOperationId);
        return s.operationId == null && s.currentOperationId == null && "saved_pending".equals(s.phase) &&
            s.terminal && s.retrySafe;
    }

    static final class View {
        final Phase phase;
        final String title, detail;
        final boolean canApply, poll;
        final long proofExpiryDelayMs;
        View(Phase phase, String title, String detail, boolean canApply, boolean poll, long expiry) {
            this.phase = phase; this.title = title; this.detail = detail;
            this.canApply = canApply; this.poll = poll; this.proofExpiryDelayMs = expiry;
        }
    }

    static boolean uuid(String id) {
        return id != null && id.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");
    }
    private static boolean epoch(String value) {
        if (value == null || !value.matches("[1-9][0-9]{0,19}")) return false;
        return value.length() < 20 || value.compareTo("18446744073709551615") <= 0;
    }
    private static boolean running(String phase) {
        return "queued".equals(phase) || "preflight".equals(phase) ||
            "reconfiguring".equals(phase) || "restoring".equals(phase) ||
            "verifying".equals(phase) || "waiting_for_playback".equals(phase);
    }
    private static boolean known(String phase) {
        return "saved_pending".equals(phase) || running(phase) || "applied".equals(phase) || "disabled".equals(phase) || "unconfirmed".equals(phase) ||
            "rejected".equals(phase) || "superseded".equals(phase) || "recovered".equals(phase) ||
            "failed".equals(phase) || "unavailable".equals(phase);
    }
    private static String reason(Status status, String fallback) {
        // Exact backend reasons remain in Operation details, outside the primary status.
        return fallback;
    }
    private static boolean ownsCurrent(Status s, Expected e) {
        return s.operationKnownReported && s.operationKnown && s.currentOperationReported &&
            e.operationId != null && e.operationId.equals(s.currentOperationId) && uuid(s.currentBootId);
    }
    private static long proofRemaining(Status s, Expected e, long now) {
        if (!ownsCurrent(s, e) || !s.available || !s.currentContextSupported || !s.currentConfigMatches ||
            !s.applied || s.disabled || !s.terminal || !"applied".equals(s.phase) || s.pendingHq ||
            !s.fresh || !s.leaseValid || !s.newEpoch || !epoch(s.graphEpoch) ||
            e.hqMs <= 0 || s.wrFrames <= 0 || s.confirmedRewrittenWrappers <= 0 ||
            s.activeDeltaUs != e.hqMs * 1000L || s.receivedElapsedMs <= 0 ||
            now < s.receivedElapsedMs || s.observationAgeMs < 0 ||
            s.observationAgeMs >= PROOF_MS || s.leaseRemainingMs <= 0 ||
            s.leaseRemainingMs > PROOF_MS) return 0;
        long elapsed = now - s.receivedElapsedMs;
        long window = Math.min(PROOF_MS - s.observationAgeMs, s.leaseRemainingMs);
        return elapsed < window ? window - elapsed : 0;
    }
    private static long disabledRemaining(Status s, Expected e, long now) {
        if (!ownsCurrent(s, e) || !s.disableAvailable || e.hqMs != 0 || !"disabled".equals(s.phase) || !s.terminal || s.applied ||
            !s.disabled || !s.disableReceiptValid || !s.currentConfigMatches || s.pendingHq ||
            !s.fresh || !s.newSession || !s.normalOwnerVerified || !s.oldRelayRemoved || !s.stockGslVerified ||
            s.observationAgeMs < 0 || s.observationAgeMs >= PROOF_MS) return 0;
        long elapsed = now - s.receivedElapsedMs;
        long window = PROOF_MS - s.observationAgeMs;
        return elapsed < window ? window - elapsed : 0;
    }

    static View evaluate(Expected expected, Status status, long now, boolean sessionUsable, boolean submitting) {
        if (expected == null) throw new IllegalArgumentException("Expected saved delay is required");
        if (!sessionUsable) return new View(Phase.UNAVAILABLE, "Module connection unavailable",
            "Reconnect Shizuka to check this operation.", false, false, 0);
        if (status == null) return new View(expected.operationId == null ? Phase.SAVED : Phase.UNCONFIRMED,
            expected.operationId == null ? "Saved request" : "Checking delay operation",
            expected.operationId == null ? "Refresh to check whether application is available." :
                "The launch response is unknown. Check this operation before trying again.",
            false, expected.operationId != null, 0);
        if (status.schema != 1 || !"latency_application".equals(status.scope) ||
            status.rootGeneration != expected.rootGeneration || status.receivedElapsedMs <= 0 ||
            now < status.receivedElapsedMs)
            return new View(Phase.UNCONFIRMED, "Operation status unavailable",
                "Refresh this module session before applying saved values.", false, true, 0);
        if (!known(status.phase)) return new View(Phase.UNCONFIRMED, "Unknown operation status",
            "Refresh or update the module. No application has been confirmed.", false, true, 0);

        boolean currentOwner = status.operationKnownReported && status.currentOperationReported && uuid(status.currentBootId) &&
            (status.currentOperationId == null || (uuid(status.currentOperationId) && status.currentOperationId.equals(status.operationId)));
        boolean qualified = currentOwner && (expected.hqMs == 0 ? status.disableAvailable :
            status.available && status.currentContextSupported) && !submitting && status.retrySafe &&
            status.terminal && now - status.receivedElapsedMs < PROOF_MS &&
            !"failed".equals(status.phase) && !"unavailable".equals(status.phase);
        boolean busy = status.operationId != null && !status.terminal;
        if ((status.operationId != null && !uuid(status.operationId)) ||
            (status.operationId == null && !"saved_pending".equals(status.phase) && !"unavailable".equals(status.phase)))
            return new View(Phase.UNCONFIRMED, "Operation identity unavailable",
                "Refresh this module session before applying saved values.", false, true, 0);
        if (expected.operationId == null) return new View(busy ? Phase.PENDING : Phase.SAVED,
            busy ? "A delay operation is running" : "Saved request",
            reason(status, qualified ? "Apply the saved values to a new session. Your codec choice is kept." :
                "Application is not available for the current session. Your codec choice is kept."),
            qualified && !busy, busy, qualified ? PROOF_MS - (now - status.receivedElapsedMs) + 1 : 0);
        if (!uuid(status.operationId) || !expected.operationId.equals(status.operationId))
            return new View(Phase.UNCONFIRMED, "Waiting for this operation",
                "A response for another operation cannot confirm these saved values.", false, true, 0);
        if (status.requestedHqMs != expected.hqMs || status.requestedLlMs != expected.llMs ||
            !status.currentConfigMatches)
            return new View(Phase.SUPERSEDED, "Request is no longer current",
                reason(status, status.terminal ? "Apply the current saved values explicitly when you are ready." :
                    "Waiting for the previous operation to finish its owned recovery."),
                qualified && status.terminal, !status.terminal, 0);

        long remaining = proofRemaining(status, expected, now);
        if (remaining > 0) {
            String detail = "R3 HQ at 44.1 kHz: completed buffers confirm the packet timing change. Receiver latency is unmeasured.";
            if (expected.llMs > 0 || status.pendingLl) detail += " Adaptive LL remains pending.";
            return new View(Phase.APPLIED, "Applied HQ +" + expected.hqMs + " ms", detail, false, true, remaining + 1);
        }
        remaining = disabledRemaining(status, expected, now);
        if (remaining > 0) {
            String detail = "The normal audio path is verified in a new session. Packet and receiver latency are unmeasured.";
            if (expected.llMs > 0 || status.pendingLl) detail += " Adaptive LL remains pending.";
            return new View(Phase.DISABLED, "Additional HQ delay disabled", detail, false, true, remaining + 1);
        }

        switch (status.phase) {
            case "saved_pending":
                return new View(Phase.UNCONFIRMED, "Previous operation unavailable",
                    "The module has no record of this operation. Reopen the module to check the current request.",
                    false, true, 0);
            case "queued": case "preflight":
                return new View(Phase.PENDING, "Checking saved delay",
                    reason(status, "The module is checking the current codec and session."), false, true, 0);
            case "reconfiguring": case "restoring":
                return new View(Phase.RECONFIGURING, "Reconfiguring audio",
                    reason(status, "Your codec choice is kept. Playback may briefly disconnect."), false, true, 0);
            case "verifying":
                return new View(Phase.VERIFYING, "Checking the new session",
                    reason(status, "Waiting for current completed-buffer proof."), false, true, 0);
            case "waiting_for_playback":
                return new View(Phase.WAITING_FOR_PLAYBACK, "Waiting for playback",
                    reason(status, "Start playback when you are ready. Shizuka will not press Play."), false, true, 0);
            case "applied": case "unconfirmed":
                return new View(Phase.UNCONFIRMED, "Application unconfirmed",
                    reason(status, "Current completed-buffer proof is missing or expired."),
                    qualified && status.terminal, true, 0);
            case "disabled":
                return new View(Phase.UNCONFIRMED, "Disabling unconfirmed",
                    reason(status, "The normal-session receipt is missing or expired."),
                    qualified && status.terminal, true, 0);
            case "rejected":
                return new View(Phase.REJECTED, "Delay request not applied",
                    reason(status, "The current codec or session is not supported. Your codec choice is kept."),
                    qualified && status.terminal, !status.terminal, 0);
            case "superseded":
                return new View(Phase.SUPERSEDED, "Operation superseded",
                    reason(status, "A user or session change ended this operation."),
                    qualified && status.terminal, !status.terminal, 0);
            case "recovered":
                return new View(Phase.RECOVERED, "Previous session recovered",
                    reason(status, "Recovery finished. Saved delay values are kept."),
                    qualified && status.terminal, !status.terminal, 0);
            case "failed":
                return new View(Phase.FAILED, "Delay operation failed",
                    reason(status, "Check module status before starting another operation."),
                    false, !status.terminal, 0);
            case "unavailable":
                return new View(Phase.UNAVAILABLE, "Delay application unavailable",
                    reason(status, "The module has no qualified executor for this operation."), false, false, 0);
            default:
                return new View(Phase.UNCONFIRMED, "Unknown operation status",
                    "Refresh or update the module. No application has been confirmed.", false, true, 0);
        }
    }
}
