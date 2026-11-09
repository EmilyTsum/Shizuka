// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.BluetoothAdapter;
import java.lang.reflect.*;
import java.util.Collections;
import java.util.Set;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Root-only fixed controller features READ. Empty request; no setters, retries or follow-up reads.
 */
public final class ControllerFeatures {
    static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder();
        for (byte b : bytes)
            out.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return out.toString();
    }
    static BluetoothAdapter setup() throws Exception {
        return BluetoothRuntime.connect().adapter;
    }
    public static void main(String[] args) {
        if (android.os.Process.myUid() != 0 || args.length != 0) {
            System.err.println("Requires UID0 and no arguments");
            System.exit(64);
            return;
        }
        final long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        Thread watchdog = new Thread(() -> {
            try {
                Thread.sleep(10000);
            } catch (InterruptedException e) {
                return;
            }
            System.err.println("controller features request hard timeout; no fallback sent");
            System.exit(124);
        }, "addon-read-deadline");
        watchdog.setDaemon(true);
        watchdog.start();
        ExecutorService executor = Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "addon-read-callback");
            t.setDaemon(true);
            return t;
        });
        CountDownLatch received = new CountDownLatch(1);
        AtomicReference<String> failure = new AtomicReference<>();
        AtomicReference<byte[]> reply = new AtomicReference<>();
        BluetoothAdapter adapter = null;
        Object callback = null;
        Method unregister = null;
        boolean registered = false;
        int exit = 2;
        try {
            adapter = setup();
            if (adapter == null || !adapter.isEnabled())
                throw new IllegalStateException("Bluetooth is not enabled");
            Class<?> api = Class.forName(
                "android.bluetooth.BluetoothAdapter$BluetoothHciVendorSpecificCallback");
            callback = Proxy.newProxyInstance(
                api.getClassLoader(), new Class<?>[] {api}, (proxy, method, a) -> {
                    switch (method.getName()) {
                    case "hashCode":
                        return System.identityHashCode(proxy);
                    case "equals":
                        return proxy == a[0];
                    case "toString":
                        return "ControllerFeaturesCallback";
                    case "onCommandComplete":
                        if (((Integer)a[0]) != ControllerFeaturesSchema.OCF)
                            failure.compareAndSet(null, "unexpected completion OCF");
                        else if (a[1] == null)
                            failure.compareAndSet(null, "null completion");
                        else
                            reply.compareAndSet(null, ((byte[])a[1]).clone());
                        received.countDown();
                        break;
                    case "onCommandStatus":
                        if (((Integer)a[0]) != ControllerFeaturesSchema.OCF ||
                            ((Integer)a[1]) != 0) {
                            if (((Integer)a[0]) == ControllerFeaturesSchema.OCF)
                                reply.compareAndSet(null, new byte[] {((Integer)a[1]).byteValue()});
                            else
                                failure.compareAndSet(null,
                                                      "unexpected command status OCF=" + a[0]);
                            received.countDown();
                        }
                        break;
                    case "onEvent":
                    case "onAclEvent":
                        failure.compareAndSet(null, "unexpected event on empty registration");
                        received.countDown();
                        break;
                    default:
                        throw new UnsupportedOperationException(method.getName());
                    }
                    return null;
                });
            Method register = BluetoothAdapter.class.getMethod(
                "registerBluetoothHciVendorSpecificCallback", Set.class, Executor.class, api);
            unregister = BluetoothAdapter.class.getMethod(
                "unregisterBluetoothHciVendorSpecificCallback", api);
            register.invoke(adapter, Collections.emptySet(), executor, callback);
            registered = true;
            // OCF0x1D is accepted by the existing public API and Binder policy.
            // Keep all existing privilege checks and registered-cookie queue semantics.
            byte[] request = ControllerFeaturesSchema.request();
            if (!ControllerFeaturesSchema.permit(android.os.Process.myUid(),
                                                 ControllerFeaturesSchema.OCF, request))
                throw new AssertionError("request guard");
            if (System.nanoTime() >= deadline - TimeUnit.SECONDS.toNanos(2))
                throw new TimeoutException("setup exceeded deadline; no command sent");
            BluetoothAdapter
                .class.getMethod("sendBluetoothHciVendorSpecificCommand", int.class, byte[].class)
                .invoke(adapter, ControllerFeaturesSchema.OCF, request);
            long remaining = deadline - System.nanoTime() - TimeUnit.SECONDS.toNanos(2);
            if (remaining <= 0 || !received.await(remaining, TimeUnit.NANOSECONDS))
                throw new TimeoutException("no completion");
            if (failure.get() != null)
                throw new IllegalStateException(failure.get());
            byte[] response = reply.get();
            int status = ControllerFeaturesSchema.status(ControllerFeaturesSchema.OCF, response);
            boolean accepted =
                ControllerFeaturesSchema.accepted(ControllerFeaturesSchema.OCF, response);
            System.out.println(ControllerFeaturesSchema.json(response));
            exit = accepted ? 0 : 4;
        } catch (Throwable e) {
            Throwable cause =
                e instanceof InvocationTargetException && e.getCause() != null ? e.getCause() : e;
            System.err.println("controller features request failed: " + cause);
        } finally {
            if (registered)
                try {
                    unregister.invoke(adapter, callback);
                } catch (Throwable e) {
                    System.err.println("Unregister failed: " + e);
                    exit = 3;
                }
            executor.shutdownNow();
        }
        System.exit(exit);
    }
}
