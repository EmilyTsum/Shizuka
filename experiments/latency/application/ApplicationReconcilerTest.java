// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.*;

public final class ApplicationReconcilerTest {
    static int checks;static final String OTHER_BOOT="055c0fe3-d91c-44ab-8700-5220f57d0d66";
    interface Task{void run()throws Exception;}
    static void check(boolean yes){checks++;if(!yes)throw new AssertionError("reconcile check "+checks);}
    static void rejects(Task task)throws Exception {try{task.run();throw new AssertionError("Unverified orphan resolution");}catch(IOException|IllegalArgumentException expected){checks++;}}
    static ApplicationReconciler.Normal normal(String boot) {
        ApplicationReconciler.Normal n=new ApplicationReconciler.Normal();n.boot=boot;n.start="200";n.hal=42;n.inputsHash=RootFactsTest.HASH;n.observedAt=1000;n.verified=true;return n;
    }
    static ApplicationReconciler.Bundle bundle()throws Exception {
        RootFactsTest.Fake f=new RootFactsTest.Fake(150,0);ApplicationReconciler.Bundle b=new ApplicationReconciler.Bundle();b.record=ApplicationOwnerWriterTest.journal(f);
        b.record.sequence=5;b.record.phase=ApplyOperation.Phase.RECONFIGURING;b.record.snapshot=f.snapshot;b.captureBytes=f.captured;b.capture=ApplicationCapture.decode(f.captured);b.owner=new ApplicationOwnerRecord(f.owner);
        b.runtimeIdentity=("1 "+RootFactsTest.COOKIE+" "+RootFactsTest.HASH+" "+RootFactsTest.HASH+" "+RootFactsTest.HASH+"\n").getBytes(StandardCharsets.US_ASCII);return b;
    }
    static List<ApplicationReconciler.Resolution> verify(ApplicationReconciler.Bundle b,ApplicationReconciler.Normal n,ApplicationReconciler.Probe p)throws Exception {return ApplicationReconciler.verify(Collections.singletonList(b),n.boot,RootFactsTest.HASH,1001,n,p);}
    static Map<String,Object> decoded(Map<String,Object> m)throws Exception{return BoundedJson.object(ControlJson.encode(m));}
    public static void main(String[] args)throws Exception {
        ApplicationReconciler.Bundle b=bundle();ApplicationReconciler.Normal n=normal(RootFactsTest.BOOT);
        List<ApplicationReconciler.Resolution> good=verify(b,n,pid->null);check(good.size()==1&&!good.get(0).newBoot&&good.get(0).originalPackage.equals(RootFactsTest.HASH));
        rejects(()->verify(b,n,pid->pid==123?"456":null));rejects(()->verify(b,n,pid->pid==41?"100":null));
        check(verify(b,n,pid->"999").size()==1); // Reused PIDs are never signalled.
        ApplicationReconciler.Normal previous=normal(OTHER_BOOT);final int[] probes={0};check(verify(b,previous,pid->{probes[0]++;throw new AssertionError("Previous boot PID queried");}).get(0).newBoot&&probes[0]==0);
        n.verified=false;rejects(()->verify(b,n,pid->null));n.verified=true;
        n.observedAt=0;rejects(()->ApplicationReconciler.verify(Collections.singletonList(b),n.boot,RootFactsTest.HASH,2000,n,pid->null));n.observedAt=1002;rejects(()->verify(b,n,pid->null));n.observedAt=1000;
        n.inputsHash="bad";rejects(()->verify(b,n,pid->null));n.inputsHash=RootFactsTest.HASH;
        n.hal=41;n.start="100";rejects(()->verify(b,n,pid->null));n.hal=42;n.start="200";
        ApplicationOwnerRecord owner=b.owner;b.owner=null;rejects(()->verify(b,n,pid->null));b.owner=owner;
        byte[] original=b.captureBytes;b.captureBytes=null;rejects(()->verify(b,n,pid->null));b.captureBytes=original;
        b.owner=new ApplicationOwnerRecord(new String(owner.bytes(),StandardCharsets.US_ASCII).replace(" 150 0 "," 149 0 "));rejects(()->verify(b,n,pid->null));b.owner=owner;
        ApplyOperation.Snapshot snapshot=b.record.snapshot;b.record.snapshot=new ApplyOperation.Snapshot(snapshot.epoch,snapshot.token,snapshot.configIdentity,false,true,true,44100,44100,1,true,true);rejects(()->verify(b,n,pid->null));b.record.snapshot=snapshot;
        byte[] request=(RootFactsTest.BOOT+" "+RootFactsTest.COOKIE+" "+RootFactsTest.HASH+" relay\n").getBytes(StandardCharsets.US_ASCII);
        check(ApplicationReconciler.requestOwner(request,good)==good.get(0));
        for(String bad:new String[]{new String(request,StandardCharsets.US_ASCII).trim(),new String(request,StandardCharsets.US_ASCII).replace(" relay"," observer"),new String(request,StandardCharsets.US_ASCII).replace(RootFactsTest.BOOT,OTHER_BOOT),new String(request,StandardCharsets.US_ASCII).replace(RootFactsTest.HASH,"b"+RootFactsTest.HASH.substring(1))})rejects(()->ApplicationReconciler.requestOwner(bad.getBytes(StandardCharsets.US_ASCII),good));
        byte[] identity=b.runtimeIdentity;b.runtimeIdentity=null;rejects(()->ApplicationReconciler.requestOwner(request,good));b.runtimeIdentity="broken".getBytes(StandardCharsets.US_ASCII);rejects(()->ApplicationReconciler.requestOwner(request,good));b.runtimeIdentity=identity;
        Map<String,Object> receipt=decoded(ApplicationReconciler.receipt(good.get(0),n.boot,RootFactsTest.HASH,n));check(ApplicationReconciler.receiptMatches(receipt,b.record));
        b.record.phase=ApplyOperation.Phase.RECOVERED;b.record.sequence++;check(ApplicationReconciler.receiptMatches(receipt,b.record));
        b.record.sequence++;check(!ApplicationReconciler.receiptMatches(receipt,b.record));b.record.sequence--;
        Map<String,Object> changed=new LinkedHashMap<>(receipt);changed.put("requested_hq_ms",149L);check(!ApplicationReconciler.receiptMatches(changed,b.record));
        changed=new LinkedHashMap<>(receipt);changed.put("schema","1");check(!ApplicationReconciler.receiptMatches(changed,b.record));
        changed=new LinkedHashMap<>(receipt);changed.put("extra",true);check(!ApplicationReconciler.receiptMatches(changed,b.record));
        changed=new LinkedHashMap<>(receipt);changed.put("stock_normal_verified",false);check(!ApplicationReconciler.receiptMatches(changed,b.record));
        changed=new LinkedHashMap<>(receipt);changed.put("current_boot",OTHER_BOOT);check(!ApplicationReconciler.receiptMatches(changed,b.record));
        ApplicationReconciler.Bundle queued=bundle();queued.record.snapshot=null;queued.owner=null;queued.capture=null;queued.captureBytes=null;queued.runtimeIdentity=null;queued.record.phase=ApplyOperation.Phase.QUEUED;queued.record.workerPid=0;queued.record.workerStart="";
        check(verify(queued,n,pid->{throw new AssertionError("Unbound queued PID queried");}).size()==1);
        System.out.println("Application reconciler: "+checks+" stock-proof/dead-owner/old-boot/foreign-request/durable-receipt checks passed.");
    }
}
