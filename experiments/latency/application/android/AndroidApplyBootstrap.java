// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.os.Handler;
import android.os.Looper;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Resident bootstrap; completion runs on the main loop after local cleanup. */
final class AndroidApplyBootstrap {
    interface Transaction {void run(ResidentApplyAdapter adapter)throws Exception;}
    interface Completion {void finished(Exception failure);}
    static void start(ResidentApplyAdapter.Owner owner,boolean disabling,Transaction transaction,Completion completion)throws Exception {
        if(disabling?!owner.disableQualified():!owner.qualified())throw new IllegalStateException("This lifecycle operation is not qualified");
        BluetoothRuntime runtime=BluetoothRuntime.connect();Handler handler=new Handler(Looper.getMainLooper());
        AtomicBoolean finished=new AtomicBoolean(),started=new AtomicBoolean(),connected=new AtomicBoolean();
        Object lock=new Object();BluetoothProfile[] accepted={null};Set<BluetoothProfile> retired=Collections.newSetFromMap(new IdentityHashMap<BluetoothProfile,Boolean>());
        java.util.function.Consumer<BluetoothProfile> retire=profile->{
            if(profile==null)return;boolean close;synchronized(lock){close=retired.add(profile);}
            if(close)runtime.adapter.closeProfileProxy(BluetoothProfile.A2DP,profile);
        };
        java.util.function.Consumer<Exception> finish=failure->{
            if(!finished.compareAndSet(false,true))return;
            connected.set(false);BluetoothProfile profile;synchronized(lock){profile=accepted[0];}
            Exception result=failure;
            try{retire.accept(profile);}catch(Exception closeFailure){if(result==null)result=closeFailure;}
            completion.finished(result);
        };
        Runnable expire=()->{if(!started.get())finish.accept(new IllegalStateException("A2DP service did not become ready"));};
        BluetoothProfile.ServiceListener listener=new BluetoothProfile.ServiceListener() {
            public void onServiceConnected(int id,BluetoothProfile profile) {
                if(id!=BluetoothProfile.A2DP)return;
                if(profile==null){finish.accept(new IllegalStateException("A2DP service returned no profile"));return;}
                boolean run=false,close=false;
                synchronized(lock) {
                    if(finished.get())close=true;
                    else if(started.get()) {
                        // A replacement cannot authorize a captured transaction.
                        if(profile!=accepted[0]){connected.set(false);close=true;}
                    }else {accepted[0]=profile;connected.set(true);started.set(true);run=true;}
                }
                if(close){retire.accept(profile);return;}
                if(!run)return;
                handler.removeCallbacks(expire);
                new Thread(()->{
                    Exception failure=null;
                    try(AndroidPlaybackSession android=new AndroidPlaybackSession(runtime,()->{
                        synchronized(lock){if(!connected.get()||finished.get())throw new IllegalStateException("A2DP service identity disconnected");return accepted[0];}
                    },()->{
                        ResidentApplyAdapter.Context context=owner.inspect();
                        if(!connected.get()||finished.get()||!context.authority||context.ownedRecoveryRequired)throw new IllegalStateException("Resident lifecycle authority changed");
                    });ResidentApplyAdapter adapter=new ResidentApplyAdapter(owner,android)) {
                        if(!connected.get()||finished.get())throw new IllegalStateException("Resident service is no longer current");
                        transaction.run(adapter);
                    }catch(Exception error){failure=error;}
                    Exception result=failure;handler.post(()->finish.accept(result));
                },"Shizuka delay application").start();
            }
            public void onServiceDisconnected(int id){if(id==BluetoothProfile.A2DP)connected.set(false);}
        };
        if(!runtime.adapter.isEnabled())throw new IllegalStateException("Bluetooth is disabled");
        handler.postDelayed(expire,3000);
        boolean requested;
        try{requested=runtime.adapter.getProfileProxy(runtime.context,listener,BluetoothProfile.A2DP);}
        catch(Exception failure){handler.removeCallbacks(expire);finish.accept(failure);return;}
        if(!requested){handler.removeCallbacks(expire);finish.accept(new IllegalStateException("A2DP service request was rejected"));}
    }
}
