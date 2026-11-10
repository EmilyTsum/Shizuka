// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;

/** Read-only normal-control fence. Recovery is never prohibited by this model. */
final class ApplicationFence {
    static final long QUEUE_GRACE=20000,GUARDIAN_LIMIT=360000;
    interface Probe {boolean alive(ApplyStore.Record record)throws Exception;}
    static Map<String,Object> evaluate(List<ApplyStore.Record> records,String current,String boot,long now,boolean known,Probe probe) {
        boolean unresolved=false,healthy=known,orphaned=false;int pending=0;String reason=known?"No unresolved application ownership":"Application metadata is unavailable or inconsistent";
        for(ApplyStore.Record r:records)if(r.unresolved()) {
            unresolved=true;pending++;long age=now-r.created;boolean alive=false;
            try{alive=r.boot.equals(boot)&&r.workerPid>1&&probe.alive(r);}catch(Exception unavailable){alive=false;}
            boolean queue=r.phase==ApplyOperation.Phase.QUEUED&&r.workerPid==0&&age>=0&&age<=QUEUE_GRACE;
            boolean inProgress=!ApplyOperation.terminal(r.phase)&&r.boot.equals(boot)&&age>=0&&age<=GUARDIAN_LIMIT&&(alive||queue);
            if(!inProgress){healthy=false;orphaned|=!alive;reason=!r.boot.equals(boot)?"Application ownership belongs to a previous boot":"Application owner is missing, expired or requires verified recovery";}
            else if(healthy)reason="A live bounded application owner retains lifecycle authority";
        }
        if(!known){unresolved=true;healthy=false;}
        return ControlJson.object("schema",1,"scope","latency_application_fence","known",known,"unresolved",unresolved,
            "pending_count",pending,"normal_mutation_allowed",known&&!unresolved,"healthy",healthy,"orphaned",orphaned,
            "current_operation_id",current,"current_boot_id",boot,"reason",reason);
    }
}
