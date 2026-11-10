// SPDX-License-Identifier: GPL-2.0-only
import java.security.SecureRandom;
import java.util.Locale;

/** Resident Android side of one transaction; fixed lifecycle owner remains a separate authority. */
final class ResidentApplyAdapter implements ApplyOperation.Adapter,AutoCloseable {
    static final class Context {
        ApplyOperation.Epoch epoch;
        String configIdentity;
        boolean authority,reviewedHq,targetEligible,normalRemovalPossible,ownedRecoveryRequired;
        int observedRate,profile;
    }
    static final class ResetPermit {
        String operationId,snapshotToken,configIdentity,expectedResetFingerprint;
        long codecEventSequence;
        boolean ownedNewProcessVerified;
        boolean valid(ApplyOperation.Request r,ApplyOperation.Snapshot s) {
            return ownedNewProcessVerified&&r.id.equals(operationId)&&s.token.equals(snapshotToken)&&s.configIdentity.equals(configIdentity)&&
                expectedResetFingerprint!=null&&expectedResetFingerprint.matches("[a-f0-9]{64}")&&codecEventSequence>=0;
        }
    }
    interface Owner {
        boolean qualified()throws Exception;
        boolean disableQualified()throws Exception;
        Context inspect()throws Exception;
        boolean requestCurrent(ApplyOperation.Request request,String configIdentity)throws Exception;
        /** For later status processes: verify the owner's private captured peer/codec binding. */
        boolean snapshotCurrent(ApplyOperation.Snapshot snapshot)throws Exception;
        void arm(ApplyOperation.Request r,ApplyOperation.Snapshot s,AndroidPlaybackSession.Captured captured,long deadline)throws Exception;
        void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception;
        void remove(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception;
        ResetPermit resetPermit(ApplyOperation.Request r,ApplyOperation.Snapshot s,long codecEventSequence)throws Exception;
        ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception;
        ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception;
        void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean waiting,long deadline)throws Exception;
        boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception;
    }
    private final Owner owner;
    private final AndroidPlaybackSession android;
    private AndroidPlaybackSession.Captured captured;
    private ApplyOperation.Snapshot snapshot;
    private boolean ownReconfiguration;
    ResidentApplyAdapter(Owner owner,AndroidPlaybackSession android){this.owner=owner;this.android=android;}
    public long now(){return System.nanoTime()/1000000L;}
    public boolean qualified()throws Exception{return owner.qualified();}
    public boolean disableQualified()throws Exception{return owner.disableQualified();}
    public boolean authority()throws Exception{return owner.inspect().authority;}
    public boolean ownedRecoveryRequired()throws Exception{return owner.inspect().ownedRecoveryRequired;}
    public boolean currentContextSupported()throws Exception {
        Context c=owner.inspect();if(!c.authority||c.ownedRecoveryRequired||!c.targetEligible)return false;
        AndroidPlaybackSession.Captured peek=android.capture(false);return peek.peer!=null&&peek.targetEligible&&peek.selectedRate==44100;
    }
    public boolean requestCurrent(ApplyOperation.Request request,ApplyOperation.Snapshot s)throws Exception {
        return owner.requestCurrent(request,s==null?null:s.configIdentity);
    }
    public ApplyOperation.Snapshot capture(ApplyOperation.Request request)throws Exception {
        Context context=owner.inspect();captured=android.capture(request.hqMs==0);
        if(!context.authority||context.ownedRecoveryRequired)throw new IllegalStateException("Lifecycle authority or remote recovery changed");
        byte[] nonce=new byte[32];new SecureRandom().nextBytes(nonce);StringBuilder token=new StringBuilder();
        for(byte b:nonce)token.append(String.format(Locale.ROOT,"%02x",b&255));
        snapshot=new ApplyOperation.Snapshot(context.epoch,token.toString(),context.configIdentity,captured.playing,captured.peer!=null,
            context.reviewedHq,captured.selectedRate,context.observedRate,context.profile,context.normalRemovalPossible,context.targetEligible&&captured.targetEligible);
        return snapshot;
    }
    public ApplyOperation.Fence fence(ApplyOperation.Snapshot s)throws Exception {
        if(captured==null)return owner.snapshotCurrent(s)?ApplyOperation.Fence.SAME:ApplyOperation.Fence.UNKNOWN;
        if(snapshot!=s)return ApplyOperation.Fence.UNKNOWN;
        return android.check(captured,ownReconfiguration);
    }
    public void armRecovery(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception {
        // Root owner persists both the recovery generation and private peer/codec
        // binding before an exact media owner can be paused.
        owner.arm(r,s,captured,deadline);android.pauseOwned(captured,deadline);
    }
    public void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception {
        ownReconfiguration=true;owner.recreate(r,s,deadline);
    }
    public void disable(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception {
        ownReconfiguration=true;owner.remove(r,s,deadline);
    }
    public void restore(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean resume,long deadline)throws Exception {
        ResetPermit permit=owner.resetPermit(r,s,android.codecEventSequence());
        android.restore(captured,resume,permit!=null&&permit.valid(r,s)?permit.expectedResetFingerprint:null,
            permit!=null&&permit.valid(r,s)?permit.codecEventSequence:-1,deadline);
        ownReconfiguration=false;
    }
    public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return owner.observe(r,s);}
    public ApplyOperation.DisabledReceipt observeDisabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return owner.disabled(r,s);}
    public void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean waiting,long deadline)throws Exception {owner.finish(r,s,waiting,deadline);}
    public boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean mayRestore,long deadline)throws Exception {
        // Never clear persisted remote uncertainty with a PID change. The owner
        // must finish its verified recovery path before any Android restoration.
        boolean recovered=owner.recover(r,s,deadline);
        if(recovered&&mayRestore&&captured!=null&&snapshot==s) {
            try {restore(r,s,captured.playing,deadline);}catch(Exception changed){return false;}
        }
        return recovered;
    }
    public void pause(long millis)throws InterruptedException{Thread.sleep(millis);}
    public void close(){android.close();}
}
