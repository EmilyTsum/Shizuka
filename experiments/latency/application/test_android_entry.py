#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual resident bootstrap/factory with synthetic Android services only."""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools

STUBS = {
    "android/os/Looper.java": r'''package android.os;
public final class Looper {
 static Looper main;public static int loops,quits;final java.util.Queue<Runnable> queue=new java.util.concurrent.ConcurrentLinkedQueue<>();final java.util.List<Runnable> delayed=new java.util.ArrayList<>();
 public static void prepareMainLooper(){if(main!=null)throw new IllegalStateException("main prepared twice");main=new Looper();}public static Looper getMainLooper(){return main;}
 public static void reset(){main=null;loops=quits=0;}public static void loop(){loops++;throw new AssertionError("Unqualified factory entered Looper");}
 public void quit(){quits++;throw new AssertionError("Main Looper cannot quit");}public void quitSafely(){quit();}
 public static void drain(){Runnable r=main.queue.poll();if(r!=null)r.run();}
 public static void expire(){java.util.List<Runnable> copy;synchronized(main.delayed){copy=new java.util.ArrayList<>(main.delayed);main.delayed.clear();}for(Runnable r:copy)r.run();}
 public static int timers(){synchronized(main.delayed){return main.delayed.size();}}
}''',
    "android/os/Handler.java": r'''package android.os;
public final class Handler {final Looper looper;public Handler(Looper l){if(l==null)throw new IllegalStateException("missing main loop");looper=l;}
 public boolean post(Runnable r){looper.queue.add(r);return true;}public boolean postDelayed(Runnable r,long ms){if(ms!=3000)throw new AssertionError("unbounded startup");synchronized(looper.delayed){looper.delayed.add(r);}return true;}
 public void removeCallbacks(Runnable r){synchronized(looper.delayed){looper.delayed.remove(r);}}
}''',
    "android/content/Context.java": "package android.content;public final class Context{}",
    "android/bluetooth/BluetoothProfile.java": r'''package android.bluetooth;public interface BluetoothProfile {int A2DP=2;interface ServiceListener {void onServiceConnected(int id,BluetoothProfile p);void onServiceDisconnected(int id);}}''',
    "android/bluetooth/BluetoothAdapter.java": r'''package android.bluetooth;
public final class BluetoothAdapter {public boolean enabled=true,request=true,throwRequest,throwClose;public int requests;public BluetoothProfile.ServiceListener listener;public final java.util.Map<BluetoothProfile,Integer> closed=new java.util.IdentityHashMap<>();public final java.util.List<String> events=java.util.Collections.synchronizedList(new java.util.ArrayList<>());
 public boolean isEnabled(){return enabled;}public boolean getProfileProxy(android.content.Context c,BluetoothProfile.ServiceListener l,int id){requests++;listener=l;if(throwRequest)throw new IllegalStateException("request failed");return request;}
 public void closeProfileProxy(int id,BluetoothProfile p){synchronized(closed){closed.put(p,closed.getOrDefault(p,0)+1);}events.add("profile-close");if(throwClose)throw new IllegalStateException("close failed");}
 public int closes(BluetoothProfile p){synchronized(closed){return closed.getOrDefault(p,0);}}
}''',
    "BluetoothRuntime.java": r'''final class BluetoothRuntime {static int connects;static BluetoothRuntime current;final android.content.Context context=new android.content.Context();final android.bluetooth.BluetoothAdapter adapter=new android.bluetooth.BluetoothAdapter();static BluetoothRuntime connect(){connects++;android.os.Looper.prepareMainLooper();return current;}}''',
    "AndroidPlaybackSession.java": r'''final class AndroidPlaybackSession implements AutoCloseable {
 interface Profile{android.bluetooth.BluetoothProfile current()throws Exception;}interface Authority{void check()throws Exception;}final BluetoothRuntime runtime;final Profile profile;final Authority authority;boolean closed;
 AndroidPlaybackSession(BluetoothRuntime r,Profile p,Authority a){runtime=r;profile=p;authority=a;}
 void check()throws Exception{authority.check();if(profile.current()==null)throw new IllegalStateException("no profile");}
 public void close(){if(!closed){closed=true;runtime.adapter.events.add("session-close");}}
}''',
    "ResidentApplyAdapter.java": r'''final class ResidentApplyAdapter implements ApplyOperation.Adapter,AutoCloseable {
 static final class Context{boolean authority=true,ownedRecoveryRequired;}interface Owner {boolean qualified()throws Exception;boolean disableQualified()throws Exception;Context inspect()throws Exception;}
 final AndroidPlaybackSession session;ResidentApplyAdapter(Owner o,AndroidPlaybackSession a){session=a;}void check()throws Exception{session.check();}public void close(){session.close();}
 public long now(){throw new AssertionError();}public boolean qualified(){throw new AssertionError();}public boolean authority(){throw new AssertionError();}public boolean requestCurrent(ApplyOperation.Request r,ApplyOperation.Snapshot s){throw new AssertionError();}public ApplyOperation.Snapshot capture(ApplyOperation.Request r){throw new AssertionError();}public ApplyOperation.Fence fence(ApplyOperation.Snapshot s){throw new AssertionError();}
 public void armRecovery(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw new AssertionError();}public void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw new AssertionError();}public void restore(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean p,long d){throw new AssertionError();}public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s){throw new AssertionError();}public void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean w,long d){throw new AssertionError();}public boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean p,long d){throw new AssertionError();}public void pause(long ms){throw new AssertionError();}
}''',
    "ReadOnlyRootFacts.java": r'''final class ReadOnlyRootFacts implements ReadOnlyApplyAdapter.Facts {
 static int opens;ReadOnlyRootFacts(java.nio.file.Path p,String id,int pid,String ticks){opens++;throw new AssertionError("Unqualified factory opened production facts");}
 void requireResidentGuardian(String id,int pid,String ticks){throw new AssertionError("Production guardian was queried");}
 public boolean qualified(){return false;}public boolean disableQualified(){return false;}public boolean authority(){return false;}public boolean ownedRecoveryRequired(){return true;}public boolean currentContextSupported(){return false;}
 public boolean requestCurrent(ApplyOperation.Request r,String c){return false;}public boolean snapshotCurrent(ApplyOperation.Snapshot s){return false;}public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s){return null;}public ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s){return null;}
}''',
    "FixedLifecycleOwner.java": r'''final class FixedLifecycleOwner implements ResidentApplyAdapter.Owner {
 FixedLifecycleOwner(java.nio.file.Path p,String id,ReadOnlyRootFacts f){throw new AssertionError("Production mutation channel was constructed");}
 public boolean qualified(){return false;}public boolean disableQualified(){return false;}public ResidentApplyAdapter.Context inspect(){return new ResidentApplyAdapter.Context();}
}''',
}

TEST = r'''
import android.bluetooth.*;import android.os.*;import java.util.concurrent.*;
public final class AndroidEntryTest {
 static int checks;interface Task{void run()throws Exception;}static void check(boolean v){checks++;if(!v)throw new AssertionError("bootstrap check "+checks);}static void rejects(Task t)throws Exception{try{t.run();throw new AssertionError("Unqualified service start");}catch(IllegalStateException expected){checks++;}}
 static final class Profile implements BluetoothProfile{}
 static final class Owner implements ResidentApplyAdapter.Owner {boolean positive=true,zero=true;final ResidentApplyAdapter.Context context=new ResidentApplyAdapter.Context();public boolean qualified(){return positive;}public boolean disableQualified(){return zero;}public ResidentApplyAdapter.Context inspect(){return context;}}
 static final class Fixture {final Thread main=Thread.currentThread();final Owner owner=new Owner();final BluetoothRuntime runtime=new BluetoothRuntime();volatile int completions,transactions;volatile Exception error;
  Fixture(){Looper.reset();BluetoothRuntime.current=runtime;BluetoothRuntime.connects=0;}
  void start(AndroidApplyBootstrap.Transaction transaction)throws Exception{AndroidApplyBootstrap.start(owner,false,a->{transactions++;transaction.run(a);},e->{check(Thread.currentThread()==main);error=e;completions++;runtime.adapter.events.add("completion");});}
  void connected(Profile p){runtime.adapter.listener.onServiceConnected(2,p);}void await()throws Exception{long until=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);while(completions==0&&System.nanoTime()<until){Looper.drain();Thread.sleep(1);}check(completions==1);}
 }
 public static void main(String[] args)throws Exception {
  Fixture f=new Fixture();f.owner.positive=false;final Fixture unqualified=f;rejects(()->unqualified.start(a->{}));check(BluetoothRuntime.connects==0&&f.runtime.adapter.requests==0);
  f=new Fixture();f.owner.zero=false;final Fixture noZero=f;rejects(()->AndroidApplyBootstrap.start(noZero.owner,true,a->{},e->{}));check(BluetoothRuntime.connects==0);
  f=new Fixture();f.runtime.adapter.enabled=false;final Fixture off=f;rejects(()->off.start(a->{}));check(f.runtime.adapter.requests==0&&f.completions==0&&Looper.timers()==0);
  f=new Fixture();f.runtime.adapter.request=false;f.start(a->{});check(f.completions==1&&f.error!=null&&f.transactions==0&&Looper.timers()==0);
  f=new Fixture();f.runtime.adapter.throwRequest=true;f.start(a->{});check(f.completions==1&&f.error!=null&&f.transactions==0&&Looper.timers()==0);
  f=new Fixture();f.start(a->{});Profile wrong=new Profile();f.runtime.adapter.listener.onServiceConnected(3,wrong);check(f.transactions==0);Looper.expire();check(f.completions==1&&f.error!=null&&f.runtime.adapter.closes(wrong)==0);
  Profile late=new Profile();f.connected(late);f.connected(late);check(f.runtime.adapter.closes(late)==1&&f.completions==1);
  f=new Fixture();final Fixture success=f;f.start(a->{a.check();success.runtime.adapter.events.add("transaction");});Profile p=new Profile();f.connected(p);f.connected(p);f.await();check(f.error==null&&f.transactions==1&&f.runtime.adapter.closes(p)==1&&Looper.timers()==0);
  java.util.List<String> events=f.runtime.adapter.events;check(events.indexOf("transaction")<events.indexOf("session-close")&&events.indexOf("session-close")<events.indexOf("profile-close")&&events.indexOf("profile-close")<events.indexOf("completion"));f.connected(p);check(f.runtime.adapter.closes(p)==1&&f.completions==1&&Looper.quits==0);
  f=new Fixture();f.start(a->{throw new IllegalStateException("transaction failed");});p=new Profile();f.connected(p);f.await();check(f.error!=null&&f.runtime.adapter.closes(p)==1&&f.runtime.adapter.events.indexOf("session-close")<f.runtime.adapter.events.indexOf("completion"));
  f=new Fixture();f.owner.context.authority=false;f.start(a->a.check());p=new Profile();f.connected(p);f.await();check(f.error!=null&&f.runtime.adapter.closes(p)==1);
  f=new Fixture();CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);final CountDownLatch enter1=entered,release1=release;f.start(a->{enter1.countDown();if(!release1.await(1,TimeUnit.SECONDS))throw new AssertionError("release");a.check();});p=new Profile();f.connected(p);check(entered.await(1,TimeUnit.SECONDS));f.runtime.adapter.listener.onServiceDisconnected(2);f.connected(p);release.countDown();f.await();check(f.error!=null&&f.transactions==1&&f.runtime.adapter.closes(p)==1);
  f=new Fixture();entered=new CountDownLatch(1);release=new CountDownLatch(1);final CountDownLatch enter2=entered,release2=release;f.start(a->{enter2.countDown();if(!release2.await(1,TimeUnit.SECONDS))throw new AssertionError("release");a.check();});p=new Profile();f.connected(p);check(entered.await(1,TimeUnit.SECONDS));Profile replacement=new Profile();f.connected(replacement);check(f.runtime.adapter.closes(replacement)==1);release.countDown();f.await();check(f.error!=null&&f.transactions==1&&f.runtime.adapter.closes(p)==1);
  f=new Fixture();f.runtime.adapter.throwClose=true;f.start(a->{});p=new Profile();f.connected(p);f.await();check(f.error!=null&&f.runtime.adapter.closes(p)==1&&f.completions==1);
  f=new Fixture();f.start(a->{});f.runtime.adapter.listener.onServiceConnected(2,null);check(f.error!=null&&f.transactions==0&&f.completions==1);
  AndroidApplicationFactory production=new AndroidApplicationFactory(java.nio.file.Paths.get("/unused"));check(!production.enabled());rejects(()->production.readOnly(null));rejects(()->production.worker(null,null,(out,error)->{throw new AssertionError("production worker started");}));rejects(()->production.loop());check(ReadOnlyRootFacts.opens==0&&Looper.loops==0&&Looper.quits==0);
  System.out.println("Actual Android entry: "+checks+" mode-gate/loop/callback/service-loss/cleanup/factory-disabled checks passed.");
 }
}
'''

java, compiler = java_tools()
with tempfile.TemporaryDirectory(prefix="shizuka-android-entry-") as temporary:
    directory = Path(temporary)
    sources = []
    for name, contents in STUBS.items():
        path = directory / "stubs" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        sources.append(path)
    harness = directory / "AndroidEntryTest.java"
    harness.write_text(TEST)
    sources.extend(p for p in HERE.glob("*.java") if not p.name.endswith("Test.java"))
    sources.extend(HERE / "android" / name for name in ("AndroidApplyBootstrap.java", "AndroidApplicationFactory.java"))
    sources.extend(ROOT / "tools/android" / name for name in ("ControlJson.java", "ExtensionConfig.java", "CodecTuning.java", "TuningCatalog.java", "TuningDefinitions.java", "ClassicQhsPolicy.java"))
    subprocess.run([*compiler, "--release", "8", "-d", str(directory / "classes"), *map(str, sources), str(harness)], check=True)
    subprocess.run([java, "-cp", str(directory / "classes"), "AndroidEntryTest"], check=True, timeout=15)
