// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import java.util.UUID;

/** Foreground observation ownership. A completion can affect only its own token. */
final class DspObservationSession {
    static final class Action {
        final String stop, start, renew;
        Action(String stop, String start, String renew) { this.stop = stop; this.start = start; this.renew = renew; }
    }
    private String token, context;
    private boolean started;
    private long renewedAt, retryAt;

    synchronized Action update(boolean eligible, String currentContext, long now) {
        String stop = null, start = null, renew = null;
        if (token != null && (!eligible || !context.equals(currentContext))) {
            stop = token; token = null; context = null; started = false; retryAt = now;
        }
        if (eligible && token == null && now >= retryAt) {
            token = UUID.randomUUID().toString(); context = currentContext;
            renewedAt = now; started = false; start = token;
        } else if (token != null && started && now - renewedAt >= 1000) {
            renewedAt = now; renew = token;
        }
        return new Action(stop, start, renew);
    }
    synchronized boolean owns(String candidate) { return token != null && token.equals(candidate); }
    synchronized boolean started(String candidate, long now) {
        if (!owns(candidate)) return false;
        started = true; renewedAt = now; return true;
    }
    synchronized boolean completed(String candidate, boolean failed, long now) {
        if (!owns(candidate)) return false;
        token = null; context = null; started = false; retryAt = now + (failed ? 5000 : 250);
        return true;
    }
}
