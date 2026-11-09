// SPDX-License-Identifier: GPL-2.0-only
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/** One-shot profile discovery. The caller serializes callbacks on its event loop. */
final class ProfileSession<P> implements AutoCloseable {
    enum State {
        PENDING("pending"), CONNECTED("connected"), UNSUPPORTED("unsupported"),
        DISCONNECTED("disconnected"), TIMEOUT("timeout"), FAILED("failed"), CLOSED("closed");
        final String label;
        State(String label) { this.label = label; }
    }
    static final class Unavailable extends UnsupportedOperationException {
        final State state;
        Unavailable(int id, State state) {
            super("Bluetooth profile " + id + " unavailable: " + state.label);
            this.state = state;
        }
    }
    interface Ops<P> {
        boolean request(int id) throws Exception;
        void release(int id, P proxy) throws Exception;
        void report(int id, String operation, Exception error);
    }
    private static final class Handle<P> {
        final int id;
        final P proxy;
        boolean released;
        Handle(int id, P proxy) { this.id = id; this.proxy = proxy; }
    }
    private final Ops<P> ops;
    private final Set<Integer> requested = new LinkedHashSet<>();
    private final Set<Integer> pending = new LinkedHashSet<>();
    private final Map<Integer, P> available = new LinkedHashMap<>();
    private final Map<Integer, State> states = new LinkedHashMap<>();
    private final Map<Integer, Exception> failures = new LinkedHashMap<>();
    private final List<Handle<P>> handles = new ArrayList<>();
    private boolean started, starting, completed, closed;
    private Runnable ready;

    ProfileSession(int[] ids, Ops<P> ops) {
        this.ops = Objects.requireNonNull(ops);
        for (int id : ids) {
            if (id <= 0) throw new IllegalArgumentException("invalid Bluetooth profile: " + id);
            requested.add(id);
            states.put(id, State.PENDING);
        }
        pending.addAll(requested);
    }
    void start(Runnable ready) {
        if (started || closed) throw new IllegalStateException("profile discovery already started or closed");
        this.ready = Objects.requireNonNull(ready);
        started = starting = true;
        try {
            for (int id : requested) {
                if (closed) break;
                try {
                    if (!ops.request(id) && pending.contains(id))
                        fail(id, State.UNSUPPORTED, new Unavailable(id, State.UNSUPPORTED));
                } catch (UnsupportedOperationException | IllegalArgumentException e) {
                    Unavailable unavailable = new Unavailable(id, State.UNSUPPORTED);
                    unavailable.initCause(e);
                    fail(id, State.UNSUPPORTED, unavailable);
                } catch (Exception e) { fail(id, State.FAILED, e); }
            }
        } finally {
            starting = false;
            complete();
        }
    }
    private void fail(int id, State state, Exception error) {
        pending.remove(id);
        available.remove(id);
        states.put(id, state);
        failures.put(id, error);
        ops.report(id, state.label, error);
    }
    private void complete() {
        if (started && !starting && !completed && !closed && pending.isEmpty()) {
            completed = true;
            ready.run();
        }
    }
    private Handle<P> handle(int id, P proxy) {
        for (Handle<P> handle : handles)
            if (handle.id == id && handle.proxy == proxy) return handle;
        Handle<P> handle = new Handle<>(id, proxy);
        handles.add(handle);
        return handle;
    }
    private void release(Handle<P> handle) {
        if (handle.released) return;
        handle.released = true;
        try { ops.release(handle.id, handle.proxy); }
        catch (Exception e) { ops.report(handle.id, "close", e); }
    }
    void connected(int id, P proxy) {
        if (proxy == null) {
            if (started && !completed && !closed && requested.contains(id)) {
                fail(id, State.FAILED, new IllegalStateException("null Bluetooth profile proxy"));
                complete();
            }
            return;
        }
        Handle<P> handle = handle(id, proxy);
        if (!started || closed || completed || !requested.contains(id)) {
            // Duplicate callbacks for an owned proxy must not close a live snapshot.
            if (closed || available.get(id) != proxy) release(handle);
            return;
        }
        available.put(id, proxy);
        states.put(id, State.CONNECTED);
        failures.remove(id);
        pending.remove(id);
        complete();
    }
    void disconnected(int id) {
        if (!started || completed || closed || !requested.contains(id)) return;
        fail(id, State.DISCONNECTED, new Unavailable(id, State.DISCONNECTED));
        // Keep the handle until close, even when its service has disconnected.
        complete();
    }
    void expire() {
        if (!started || completed || closed) return;
        for (int id : new ArrayList<>(pending))
            fail(id, State.TIMEOUT, new Unavailable(id, State.TIMEOUT));
        complete();
    }
    P get(int id) { return available.get(id); }
    P require(int id) throws Exception {
        if (closed) throw new Unavailable(id, State.CLOSED);
        P proxy = get(id);
        if (proxy != null) return proxy;
        if (failures.containsKey(id)) throw failures.get(id);
        throw new Unavailable(id, states.getOrDefault(id, State.UNSUPPORTED));
    }
    public void close() {
        if (closed) return;
        closed = true;
        pending.clear();
        available.clear();
        for (Handle<P> handle : new ArrayList<>(handles)) release(handle);
    }
}
