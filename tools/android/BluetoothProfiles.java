// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.BluetoothProfile;
import android.os.Handler;
import android.os.Looper;

/** Android callback/timeout bridge for a single settings operation or status snapshot. */
final class BluetoothProfiles implements AutoCloseable {
    interface Reporter {
        void report(int id, String operation, Exception error);
    }
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ProfileSession<BluetoothProfile> session;
    private final BluetoothProfile.ServiceListener listener;
    private final Runnable timeout;

    BluetoothProfiles(BluetoothRuntime runtime, int[] ids, Reporter reporter) {
        session = new ProfileSession<>(ids, new ProfileSession.Ops<BluetoothProfile>() {
            public boolean request(int id) {
                return runtime.adapter.getProfileProxy(runtime.context, listener, id);
            }
            public void release(int id, BluetoothProfile proxy) {
                runtime.adapter.closeProfileProxy(id, proxy);
            }
            public void report(int id, String operation, Exception error) {
                reporter.report(id, operation, error);
            }
        });
        listener = new BluetoothProfile.ServiceListener() {
            public void onServiceConnected(int id, BluetoothProfile proxy) {
                handler.post(() -> session.connected(id, proxy));
            }
            public void onServiceDisconnected(int id) {
                handler.post(() -> session.disconnected(id));
            }
        };
        timeout = session::expire;
    }
    void start(long timeoutMillis, Runnable ready) {
        if (timeoutMillis <= 0) throw new IllegalArgumentException("profile timeout must be positive");
        if (!handler.postDelayed(timeout, timeoutMillis))
            throw new IllegalStateException("Bluetooth event loop is unavailable");
        try {
            session.start(() -> {
                handler.removeCallbacks(timeout);
                ready.run();
            });
        } catch (RuntimeException e) {
            close();
            throw e;
        }
    }
    BluetoothProfile get(int id) { return session.get(id); }
    BluetoothProfile require(int id) throws Exception { return session.require(id); }
    public void close() {
        handler.removeCallbacks(timeout);
        session.close();
    }
}
