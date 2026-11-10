// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.Path;

/** Concrete installed read-only implementation. Mutation capability is compiled false. */
final class ReadOnlyRootFacts implements FixedLifecycleOwner.Facts,ReadOnlyApplyAdapter.Facts {
    private final RootFacts model;private final InstalledRootSource source;private final String operationId;
    ReadOnlyRootFacts(Path module,String operationId,int guardian,String guardianStart)throws Exception {
        if(operationId!=null&&!operationId.matches(ApplicationInputs.UUID))throw new IllegalArgumentException("Invalid fixed operation identity");
        this.operationId=operationId;source=new InstalledRootSource(module,guardian,guardianStart);model=new RootFacts(source);
    }
    public boolean qualified(){return false;}
    public boolean disableQualified(){return false;}
    public boolean authority()throws Exception{return model.inspect().authority;}
    public boolean ownedRecoveryRequired()throws Exception{return model.inspect().recovery;}
    public boolean currentContextSupported()throws Exception {RootFacts.Context c=model.inspect();return c.authority&&!c.recovery&&c.targetEligible;}
    void requireResidentGuardian(String id,int pid,String start)throws Exception {source.requireResidentGuardian(id,pid,start);}
    public ResidentApplyAdapter.Context inspect()throws Exception {
        RootFacts.Context c=model.inspect();ResidentApplyAdapter.Context out=new ResidentApplyAdapter.Context();
        out.epoch=c.epoch;out.configIdentity=c.config.hash;out.authority=c.authority;out.ownedRecoveryRequired=c.recovery;
        out.reviewedHq=c.reviewedHq;out.targetEligible=c.targetEligible;out.observedRate=c.observedRate;out.profile=c.profile;
        out.normalRemovalPossible=c.normalRemovalPossible;return out;
    }
    public boolean requestCurrent(ApplyOperation.Request r,String hash)throws Exception{return operationId!=null&&operationId.equals(r.id)&&model.requestCurrent(r,hash);}
    private ApplyOperation.Request request()throws Exception {
        if(operationId==null||!operationId.equals(source.currentOperation()))return null;
        ApplicationOwnerRecord record=ApplicationOwnerRecord.decode(source.file("state/latency-application/owned."+operationId,1024,true));
        return new ApplyOperation.Request(record.id,record.hq,record.ll);
    }
    public boolean snapshotCurrent(ApplyOperation.Snapshot s)throws Exception {ApplyOperation.Request r=request();return r!=null&&model.snapshotCurrent(r,s);}
    public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return operationId!=null&&operationId.equals(r.id)?model.observe(r,s):null;}
    public ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return operationId!=null&&operationId.equals(r.id)?model.disabled(r,s):null;}
    public ResidentApplyAdapter.ResetPermit resetPermit(ApplyOperation.Request r,ApplyOperation.Snapshot s,long events) {
        // No existing API proves the origin of a mismatching reset selection.
        // Preserve it rather than echo the caller's fingerprint/event counter.
        return null;
    }
    public boolean resolved(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean recovery)throws Exception {
        if(operationId==null||!operationId.equals(r.id))return false;
        if(recovery)return model.recovered(r,s);
        ApplyOperation.Proof proof=model.observe(r,s);if(proof!=null&&proof.applied(r,s,source.now()))return true;
        ApplyOperation.DisabledReceipt disabled=model.disabled(r,s);if(disabled!=null&&disabled.valid(r,s,source.now()))return true;
        return model.waitingReady(r,s);
    }
}
