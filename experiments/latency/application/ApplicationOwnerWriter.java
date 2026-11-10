// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;

/** Typed receipt transitions. Callers retain the inherited verified lifecycle flock. */
final class ApplicationOwnerWriter {
    interface Guard {void requireHeld(String id,int pid,String start)throws Exception;}
    static ApplicationOwnerRecord transition(int action,boolean flag,RootFacts.Source source,Guard guard,
            ApplyStore.Record journal,ApplicationCapture capture,ApplicationOwnerRecord old,long deadline)throws Exception {
        ApplyOperation.Request r=capture.request;ApplyOperation.Snapshot s=capture.snapshot;
        if(action<1||action>5||deadline<=source.now()||deadline-source.now()>210000||journal==null||!journal.request.id.equals(r.id)||!journal.request.sameValues(r)||
            !journal.boot.equals(s.epoch.boot)||!r.id.equals(source.currentOperation())||journal.workerPid<=1)throw new IOException("Receipt request/journal binding mismatch");
        guard.requireHeld(r.id,journal.workerPid,journal.workerStart);
        RootFacts model=new RootFacts(source);RootFacts.Context c=model.inspect();
        if(!c.authority||c.recovery||!s.epoch.sameAuthority(c.epoch)||action!=5&&!model.requestCurrent(r,s.configIdentity))throw new IOException("Receipt authority/config/recovery changed");
        byte[] bytes=source.file("state/latency-application/channel."+r.id+"/capture.bin",8192,true);String digest=ApplicationInputs.sha(bytes);
        if(old!=null&&(!old.id.equals(r.id)||!old.boot.equals(s.epoch.boot)||!old.cookie.equals(s.epoch.cookie)||!old.token.equals(s.token)||!old.config.equals(s.configIdentity)||old.hq!=r.hqMs||old.ll!=r.llMs||
            old.guardian!=journal.workerPid||!old.guardianStart.equals(journal.workerStart)||old.oldHal!=s.epoch.halPid||!old.oldStart.equals(s.epoch.halStart)||!old.captureHash.equals(digest)||!old.packageHash.equals(c.packageHash)))throw new IOException("Existing receipt binding changed");
        String phase;long graph=0;int hal=c.epoch.halPid;String start=c.epoch.halStart,activation=c.activationHash;
        switch(action) {
        case 1:
            if(flag||journal.phase!=ApplyOperation.Phase.PREFLIGHT||s.epoch.halPid!=hal||!s.epoch.halStart.equals(start)||s.epoch.graph!=c.epoch.graph||
                !model.captureMatches(bytes,r,s,c)||(r.hqMs==0?!s.canDisable()||!c.normalRemovalPossible:!s.supported()||!c.targetEligible))throw new IOException("Capture is not current at ARM");
            phase="armed";hal=0;start="0";
            if(old!=null){if(old.phase.equals("armed"))return old;throw new IOException("ARM cannot replace a later receipt");}break;
        case 2:
            if(flag||old==null||!old.phase.equals("armed")||journal.phase!=ApplyOperation.Phase.RECONFIGURING||r.hqMs==0||!healthyNew(c,s))throw new IOException("New owned provider is not ready");
            phase="ready";break;
        case 3:
            if(flag||old==null||!old.phase.equals("armed")||journal.phase!=ApplyOperation.Phase.RECONFIGURING||r.hqMs!=0||!model.normalVerified(c,s))throw new IOException("Normal removal is not verified");
            phase="disabled";break;
        case 4:
            if(old==null||!old.phase.matches("ready|disabled|waiting|applied")||!old.activationHash.equals(c.activationHash)||old.newHal!=hal||!old.newStart.equals(start))throw new IOException("FINISH has no matching ready owner");
            if(flag) {
                if(r.hqMs==0||s.playing||journal.phase!=ApplyOperation.Phase.RESTORING||!healthyNew(c,s)||!model.captureMatches(bytes,r,s,c))throw new IOException("Idle completion is not ready");
                phase="waiting";
            } else if(r.hqMs==0) {
                if(journal.phase!=ApplyOperation.Phase.RESTORING)throw new IOException("Normal finish is outside restoration");
                ApplyOperation.DisabledReceipt receipt=model.disabled(r,s);if(receipt==null||!receipt.valid(r,s,source.now()))throw new IOException("Normal removal receipt unavailable");phase="disabled";
            } else {
                if(journal.phase!=ApplyOperation.Phase.VERIFYING||!s.playing)throw new IOException("Positive finish is outside verification");
                ApplyOperation.Proof proof=model.observe(r,s);if(proof==null||!proof.applied(r,s,source.now()))throw new IOException("Completed WR_DONE proof unavailable");phase="applied";graph=proof.epoch.graph;
            }break;
        case 5:
            if(flag||old==null||!model.normalVerified(c,s))throw new IOException("Owned normal recovery not verified");phase="recovered";break;
        default:throw new IOException("Unknown receipt action");
        }
        if(deadline<=source.now())throw new IOException("Receipt deadline expired before publication");
        return new ApplicationOwnerRecord("1 "+r.id+" "+s.epoch.boot+" "+s.epoch.cookie+" "+s.token+" "+s.configIdentity+" "+r.hqMs+" "+r.llMs+" "+journal.workerPid+" "+journal.workerStart+" "+s.epoch.halPid+" "+s.epoch.halStart+" "+hal+" "+start+" "+graph+" "+phase+" "+digest+" "+c.packageHash+" "+activation+"\n");
    }
    static boolean healthyNew(RootFacts.Context c,ApplyOperation.Snapshot s) {
        return c.lease!=null&&c.status!=null&&c.epoch.valid()&&(c.epoch.halPid!=s.epoch.halPid||!c.epoch.halStart.equals(s.epoch.halStart))&&
            c.status.error==0&&c.status.worker&&c.status.registered&&c.status.stage<=10&&c.status.stage!=7;
    }
}
