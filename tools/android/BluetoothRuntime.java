// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.content.Context;
import android.os.Looper;
import java.lang.reflect.InvocationTargetException;

/** Shared Android service bootstrap; no device-specific offsets or saved preferences. */
final class BluetoothRuntime {
    final Context context;
    final BluetoothAdapter adapter;
    private BluetoothRuntime(Context context, BluetoothAdapter adapter) {
        this.context = context; this.adapter = adapter;
    }
    static BluetoothRuntime connect() throws Exception {
        Looper.prepareMainLooper();
        Class<?> manager = Class.forName("android.os.BluetoothServiceManager");
        Class.forName("android.bluetooth.BluetoothFrameworkInitializer")
            .getMethod("setBluetoothServiceManager", manager)
            .invoke(null, manager.getConstructor().newInstance());
        Class<?> activity = Class.forName("android.app.ActivityThread");
        Object thread = activity.getMethod("systemMain").invoke(null);
        Context context = ((Context)activity.getMethod("getSystemContext").invoke(thread))
                              .createPackageContext("com.android.shell", 0);
        BluetoothManager bluetooth = context.getSystemService(BluetoothManager.class);
        if (bluetooth == null) throw new IllegalStateException("Bluetooth service unavailable");
        BluetoothAdapter adapter = bluetooth.getAdapter();
        if (adapter == null) throw new IllegalStateException("Bluetooth adapter unavailable");
        return new BluetoothRuntime(context, adapter);
    }
    static Object call(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        try { return target.getClass().getMethod(name, types).invoke(target, args); }
        catch (NoSuchMethodException e) { throw new UnsupportedOperationException("Bluetooth API unavailable: " + name, e); }
        catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            if (cause instanceof Error) throw (Error)cause;
            throw new IllegalStateException("Bluetooth API failed: " + name, cause);
        }
    }
}
