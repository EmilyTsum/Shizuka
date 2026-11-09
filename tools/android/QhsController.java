// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.content.*;
import android.os.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.Executor;

/** The only resident policy is the explicitly opted-in controller extension. */
final class QhsController {
    private final BluetoothAdapter adapter;
    private final Context context;
    private final ExtensionConfigStore store;
    private final ClassicQhsPolicy policy;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private static void say(String value) { System.out.println(value); }
    QhsController(BluetoothRuntime runtime, ExtensionConfigStore store) {
        this.adapter = runtime.adapter; this.context = runtime.context; this.store = store;
        policy = new ClassicQhsPolicy(new ClassicQhsPolicy.Ops() {
            Object callback;
            Method unregister;
            public long nowNanos() { return System.nanoTime(); }
            public void report(String value) { say(value); }
            public void register(long token) throws Exception {
                if (android.os.Process.myUid() != 0 || !adapter.isEnabled())
                    throw new IllegalStateException("root and initialized adapter required");
                Class<?> api = Class.forName(
                    "android.bluetooth.BluetoothAdapter$BluetoothHciVendorSpecificCallback");
                callback = Proxy.newProxyInstance(
                    api.getClassLoader(), new Class<?>[] {api}, (proxy, method, args) -> {
                        switch (method.getName()) {
                        case "hashCode":
                            return System.identityHashCode(proxy);
                        case "equals":
                            return proxy == args[0];
                        case "toString":
                            return "ClassicQhsPolicyCallback";
                        case "onCommandComplete":
                            policy.complete(token, (Integer)args[0],
                                               args[1] == null ? null : ((byte[])args[1]).clone());
                            break;
                        case "onCommandStatus":
                            if ((Integer)args[1] != 0)
                                policy.complete(token, (Integer)args[0],
                                                   new byte[] {((Integer)args[1]).byteValue()});
                            break;
                        default:
                            break;
                        }
                        return null;
                    });
                unregister = BluetoothAdapter.class.getMethod(
                    "unregisterBluetoothHciVendorSpecificCallback", api);
                Executor executor = task -> handler.post(task);
                BluetoothAdapter.class
                    .getMethod("registerBluetoothHciVendorSpecificCallback", Set.class,
                               Executor.class, api)
                    .invoke(adapter, Collections.emptySet(), executor, callback);
            }
            public void send(byte[] request) throws Exception {
                if (android.os.Process.myUid() != 0 || !adapter.isEnabled() ||
                    !ClassicQhsPolicy.fixedRequest(request))
                    throw new IllegalStateException("invalid fixed QHS request");
                BluetoothAdapter.class
                    .getMethod("sendBluetoothHciVendorSpecificCommand", int.class, byte[].class)
                    .invoke(adapter, ClassicQhsPolicy.OCF, request);
            }
            public void unregister() throws Exception {
                Object old = callback;
                callback = null;
                if (old != null)
                    unregister.invoke(adapter, old);
            }
            public void armTimeout(long token) {
                handler.postDelayed(() -> policy.timeout(token), 5000);
            }
        });
    }
    private void update() {
        try {
            ExtensionConfig config = store.read();
            policy.update(adapter.isEnabled(), "true".equals(config.value("classic_qhs_host_aware")), config.trusted);
        } catch (Exception e) {
            policy.update(adapter.isEnabled(), false, false);
            say("qhs_config_error=" + e.getClass().getSimpleName());
        }
    }
    void watch() {
        IntentFilter events = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        context.registerReceiver(new BroadcastReceiver() {
            public void onReceive(Context context, Intent intent) {
                if (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR) != BluetoothAdapter.STATE_ON)
                    policy.serviceLost();
                else update();
            }
        }, events, Context.RECEIVER_EXPORTED);
        adapter.getProfileProxy(context, new BluetoothProfile.ServiceListener() {
            public void onServiceConnected(int id, BluetoothProfile proxy) { update(); }
            public void onServiceDisconnected(int id) { policy.serviceLost(); }
        }, 2);
        update();
        say("extension_watch=ready; device_preferences=android");
        Looper.loop();
    }
}
