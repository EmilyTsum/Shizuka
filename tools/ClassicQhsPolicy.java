// SPDX-License-Identifier: GPL-2.0-only
/** One fixed request per observed adapter/service generation. Runs on the main Looper. */
final class ClassicQhsPolicy {
    static final int OCF = 0x51;
    interface Ops {
        void register(long generation) throws Exception;
        void send(byte[] request) throws Exception;
        void unregister() throws Exception;
        void armTimeout(long generation);
        long nowNanos();
        void report(String value);
    }
    private final Ops ops;
    long generation, attemptedAtNanos;
    boolean on, optIn, trusted, attempted, pending, registered;
    Boolean accepted;
    Integer status;
    String phase = "not_observed";
    ClassicQhsPolicy(Ops ops) { this.ops = ops; }
    static boolean secureConfig(int uid, int mode) {
        return uid == 0 && (mode & 0170000) == 0100000 && (mode & 0022) == 0;
    }
    static boolean fixedRequest(byte[] p) {
        return p != null && p.length == 3 && p[0] == 1 && p[1] == 0 && p[2] == 3;
    }
    void update(boolean adapterOn, boolean enabled, boolean rootOwned) {
        optIn = enabled;
        trusted = rootOwned;
        if (!adapterOn) {
            boolean changed = on || pending || accepted != null;
            on = false;
            pending = false;
            accepted = null;
            status = null;
            release();
            phase = "adapter_unavailable";
            if (changed)
                emit();
            return;
        }
        if (!on) {
            on = true;
            generation++;
            attempted = false;
            accepted = null;
            status = null;
            attemptedAtNanos = 0;
        }
        if (!enabled || !rootOwned) {
            if (pending) {
                pending = false;
                accepted = null; // Cancellation cannot undo a command already submitted.
                status = null;
                release();
            }
            phase = enabled ? "untrusted_config" : "policy_disabled";
            emit();
            return;
        }
        if (attempted)
            return;
        attempted = true;
        pending = true;
        attemptedAtNanos = ops.nowNanos();
        phase = "request_pending";
        try {
            ops.register(generation);
            registered = true;
            ops.armTimeout(generation);
            // Never obtain opcode, transport, mode or parameter bytes from configuration.
            ops.send(new byte[] {1, 0, 3});
            emit();
        } catch (Exception e) {
            pending = false;
            accepted = null;
            phase = "request_error";
            release();
            emit();
        }
    }
    void complete(long token, int ocf, byte[] response) {
        if (!on || !pending || token != generation)
            return;
        pending = false;
        // The pinned controller's accepted setter reply is exactly status+subopcode.
        // A successful reply for another FC51 subcommand must never satisfy this request.
        boolean exactReply = response != null && response.length == 2 && response[1] == 1;
        boolean rejectedCommandStatus =
            response != null && response.length == 1 && response[0] != 0;
        if (ocf != OCF || (!exactReply && !rejectedCommandStatus)) {
            phase = "invalid_response";
            accepted = null;
        } else {
            status = response[0] & 255;
            accepted = status == 0;
            phase = accepted ? "request_accepted" : "request_rejected";
        }
        release();
        emit();
    }
    void timeout(long token) {
        if (!on || !pending || token != generation)
            return;
        pending = false;
        accepted = null; // Timeout does not establish whether the controller changed state.
        phase = "request_timeout";
        release();
        emit();
    }
    void serviceLost() { update(false, optIn, trusted); }
    private void release() {
        if (!registered)
            return;
        registered = false;
        try {
            ops.unregister();
        } catch (Exception e) {
            ops.report("classic_qhs_callback_cleanup_error=true");
        }
    }
    private void emit() {
        ops.report("classic_qhs_request={\"generation\":" + generation + ",\"adapter_on\":" + on +
                   ",\"opt_in\":" + optIn + ",\"trusted_config\":" + trusted +
                   ",\"attempted\":" + attempted + ",\"pending\":" + pending +
                   ",\"controller_accepted\":" + accepted + ",\"controller_status\":" + status +
                   ",\"phase\":\"" + phase + "\",\"attempted_monotonic_ns\":" + attemptedAtNanos +
                   ",\"observed_monotonic_ns\":" + ops.nowNanos() +
                   ",\"negotiated_qhs\":\"unverified\"}");
    }
}
