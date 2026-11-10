// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

public final class ApplicationFenceTest {
    static int checks;static void check(boolean v){checks++;if(!v)throw new AssertionError("fence check "+checks);}
    static ApplyStore.Record record(ApplyOperation.Phase phase,int pid){ApplyStore.Record r=new ApplyStore.Record();r.request=new ApplyOperation.Request(ApplyOperationTest.ID,150,0);r.boot=ApplyOperationTest.BOOT;r.created=r.updated=1000;r.phase=phase;r.workerPid=pid;r.workerStart=pid==0?"":"100";return r;}
    static Map<String,Object> view(ApplyStore.Record r,long now,boolean known,boolean alive){return ApplicationFence.evaluate(r==null?Collections.emptyList():Collections.singletonList(r),r==null?null:r.request.id,ApplyOperationTest.BOOT,now,known,x->alive);}
    static boolean yes(Map<String,Object> m,String key){return Boolean.TRUE.equals(m.get(key));}
    public static void main(String[] args)throws Exception {
        Map<String,Object> clear=view(null,1000,true,false);check(yes(clear,"normal_mutation_allowed")&&yes(clear,"healthy")&&!yes(clear,"unresolved"));
        check(!yes(view(null,1000,false,false),"normal_mutation_allowed"));check(!yes(view(null,1000,false,false),"healthy"));
        ApplyStore.Record queued=record(ApplyOperation.Phase.QUEUED,0);Map<String,Object> pending=view(queued,1000,true,false);
        check(yes(pending,"unresolved")&&!yes(pending,"normal_mutation_allowed")&&yes(pending,"healthy"));
        check(!yes(view(queued,22000,true,false),"healthy"));check(!yes(view(queued,999,true,false),"healthy"));
        for(ApplyOperation.Phase phase:new ApplyOperation.Phase[]{ApplyOperation.Phase.PREFLIGHT,ApplyOperation.Phase.RECONFIGURING,ApplyOperation.Phase.RESTORING,ApplyOperation.Phase.VERIFYING}) {
            ApplyStore.Record live=record(phase,123);check(yes(view(live,2000,true,true),"healthy")&&!yes(view(live,2000,true,true),"normal_mutation_allowed"));
            check(!yes(view(live,2000,true,false),"healthy"));check(!yes(view(live,362000,true,true),"healthy"));
        }
        for(ApplyOperation.Phase phase:new ApplyOperation.Phase[]{ApplyOperation.Phase.FAILED,ApplyOperation.Phase.UNCONFIRMED}){ApplyStore.Record failed=record(phase,123);check(!yes(view(failed,2000,true,true),"healthy")&&!yes(view(failed,2000,true,true),"normal_mutation_allowed"));}
        for(ApplyOperation.Phase phase:new ApplyOperation.Phase[]{ApplyOperation.Phase.RECOVERED,ApplyOperation.Phase.REJECTED,ApplyOperation.Phase.SUPERSEDED,ApplyOperation.Phase.APPLIED,ApplyOperation.Phase.DISABLED,ApplyOperation.Phase.WAITING_FOR_PLAYBACK})check(yes(view(record(phase,123),2000,true,false),"normal_mutation_allowed"));
        ApplyStore.Record previous=record(ApplyOperation.Phase.RECONFIGURING,123);previous.boot="055c0fe3-d91c-44ab-8700-5220f57d0d66";final int[] probes={0};
        Map<String,Object> old=ApplicationFence.evaluate(Collections.singletonList(previous),previous.request.id,ApplyOperationTest.BOOT,2000,true,r->{probes[0]++;return true;});check(!yes(old,"healthy")&&yes(old,"orphaned")&&probes[0]==0);
        System.out.println("Application fence: "+checks+" cleanup-permission/live-child/orphan/deadline/previous-boot checks passed.");
    }
}
