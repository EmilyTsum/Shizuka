// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class ApplicationOwnerWriterTest {
    static int checks;static void check(boolean value){checks++;if(!value)throw new AssertionError("owner writer check "+checks);}
    interface Checked{void run()throws Exception;}static void rejects(Checked action)throws Exception{try{action.run();throw new AssertionError("unsafe owner transition");}catch(IOException|IllegalArgumentException expected){checks++;}}
    static ApplyStore.Record journal(RootFactsTest.Fake source){ApplyStore.Record r=new ApplyStore.Record();r.request=source.request;r.boot=RootFactsTest.BOOT;r.workerPid=123;r.workerStart="456";r.phase=ApplyOperation.Phase.PREFLIGHT;return r;}
    static ApplicationOwnerWriter.Guard guard=(id,pid,start)->{if(!id.equals(RootFactsTest.ID)||pid!=123||!start.equals("456"))throw new IOException("guardian mismatch");};
    static ApplicationOwnerRecord transition(int action,boolean flag,RootFactsTest.Fake source,ApplyStore.Record journal,ApplicationOwnerRecord old)throws Exception {
        return ApplicationOwnerWriter.transition(action,flag,source,guard,journal,ApplicationCapture.decode(source.captured),old,source.time+1000);
    }
    static void set(RootFactsTest.Fake source,ApplicationOwnerRecord record){source.owner=new String(record.bytes(),StandardCharsets.US_ASCII);}
    public static void main(String[] args)throws Exception {
        RootFactsTest.Fake f=new RootFactsTest.Fake(150,100);f.owner=null;f.live(41,"100",3);ApplyStore.Record j=journal(f);
        ApplicationOwnerRecord armed=transition(1,false,f,j,null);check(armed.phase.equals("armed")&&armed.newHal==0&&armed.graph==0);
        set(f,armed);check(java.util.Arrays.equals(transition(1,false,f,j,armed).bytes(),armed.bytes()));
        rejects(()->transition(2,false,f,j,armed));
        rejects(()->transition(1,true,f,j,null));
        rejects(()->ApplicationOwnerWriter.transition(1,false,f,(id,pid,start)->{throw new IOException("not held");},j,ApplicationCapture.decode(f.captured),null,f.time+1000));
        f.live(42,"200",1);j.phase=ApplyOperation.Phase.RECONFIGURING;
        ApplicationOwnerRecord ready=transition(2,false,f,j,armed);check(ready.phase.equals("ready")&&ready.newHal==42&&ready.graph==0);set(f,ready);
        rejects(()->transition(1,false,f,j,ready));
        rejects(()->transition(3,false,f,j,armed));
        j.phase=ApplyOperation.Phase.RESTORING;rejects(()->transition(4,false,f,j,ready));
        j.phase=ApplyOperation.Phase.VERIFYING;
        ApplicationOwnerRecord applied=transition(4,false,f,j,ready);check(applied.phase.equals("applied")&&applied.graph==1&&applied.hq==150);set(f,ready);
        RootFactsTest.put(f.status,224,0,8);RootFactsTest.crc(f.status);rejects(()->transition(4,false,f,j,ready));RootFactsTest.put(f.status,224,2,8);RootFactsTest.crc(f.status);
        f.recovery=true;rejects(()->transition(4,false,f,j,ready));f.recovery=false;
        rejects(()->ApplicationOwnerWriter.transition(4,false,f,guard,j,ApplicationCapture.decode(f.captured),ready,f.time));
        f.active=false;
        ApplicationOwnerRecord recovered=transition(5,false,f,j,ready);check(recovered.phase.equals("recovered")&&recovered.graph==0);set(f,recovered);
        check(new RootFacts(f).recovered(f.request,f.snapshot));
        // User intent can change during recovery. No old config/codec is written
        // to force a match; only normal owned resources are confirmed recovered.
        f.cfg="schema=2\nadaptive_hq_ttp_delta_ms=149\nadaptive_ll_ttp_delta_ms=0\n".getBytes(StandardCharsets.US_ASCII);
        f.configJson="{\"schema\":2,\"requested\":{\"adaptive_hq_ttp_delta_ms\":149,\"adaptive_ll_ttp_delta_ms\":0},\"applied\":false}";
        f.parcel="a different manual codec selection".getBytes(StandardCharsets.US_ASCII);
        check(new RootFacts(f).recovered(f.request,f.snapshot));check(!new RootFacts(f).requestCurrent(f.request,f.snapshot.configIdentity));

        RootFactsTest.Fake zero=new RootFactsTest.Fake(0,0);zero.active=false;zero.owner("armed",0);ApplyStore.Record z=journal(zero);z.phase=ApplyOperation.Phase.RECONFIGURING;
        ApplicationOwnerRecord zeroArmed=new ApplicationOwnerRecord(zero.owner);
        ApplicationOwnerRecord disabled=transition(3,false,zero,z,zeroArmed);check(disabled.phase.equals("disabled")&&disabled.hq==0&&disabled.graph==0);set(zero,disabled);
        z.phase=ApplyOperation.Phase.RESTORING;check(transition(4,false,zero,z,disabled).phase.equals("disabled"));
        zero.normal=false;rejects(()->transition(3,false,zero,z,zeroArmed));

        RootFactsTest.Fake idle=new RootFactsTest.Fake(150,0);ApplyOperation.Snapshot original=idle.snapshot;
        idle.snapshot=new ApplyOperation.Snapshot(original.epoch,original.token,original.configIdentity,false,true,true,44100,44100,1,true);
        idle.captured=RootFactsTest.capture(idle.request,idle.snapshot,idle.parcel);idle.owner("armed",0);ApplicationOwnerRecord idleArm=new ApplicationOwnerRecord(idle.owner);
        ApplyStore.Record i=journal(idle);i.phase=ApplyOperation.Phase.RECONFIGURING;
        ApplicationOwnerRecord idleReady=transition(2,false,idle,i,idleArm);set(idle,idleReady);i.phase=ApplyOperation.Phase.RESTORING;
        RootFactsTest.put(idle.status,52,4,4);RootFactsTest.put(idle.status,88,0,8);RootFactsTest.put(idle.status,96,0,8);RootFactsTest.put(idle.status,224,0,8);RootFactsTest.crc(idle.status);
        ApplicationOwnerRecord waiting=transition(4,true,idle,i,idleReady);check(waiting.phase.equals("waiting")&&waiting.graph==0);set(idle,waiting);
        check(new RootFacts(idle).waitingReady(idle.request,idle.snapshot));
        ApplyOperation.Proof absent=new RootFacts(idle).observe(idle.request,idle.snapshot);check(absent!=null&&!absent.applied(idle.request,idle.snapshot,idle.time));
        RootFactsTest.Fake first=new RootFactsTest.Fake(150,0);first.active=false;first.live(41,"100",0);first.owner=null;
        first.snapshot=new ApplyOperation.Snapshot(ApplyOperationTest.epoch(41,"100",0),first.snapshot.token,first.snapshot.configIdentity,false,true,false,44100,0,0,true,true);
        first.captured=RootFactsTest.capture(first.request,first.snapshot,first.parcel);ApplyStore.Record firstJournal=journal(first);
        ApplicationOwnerRecord firstArm=transition(1,false,first,firstJournal,null);check(firstArm.phase.equals("armed")&&firstArm.newHal==0);
        check(!first.snapshot.reviewedHq&&first.snapshot.profile==0&&first.snapshot.observedRate==0);
        System.out.println("Owner receipt writer: "+checks+" guardian/phase/actual-WR/idle/normal-recovery checks passed.");
    }
}
