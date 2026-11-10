// SPDX-License-Identifier: GPL-2.0-only
/** Preparation/status never acquire a media token or create a mutation channel. */
final class ReadOnlyApplyAdapter implements ApplyOperation.Adapter {
    interface Facts {
        boolean qualified()throws Exception;
        boolean disableQualified()throws Exception;
        boolean authority()throws Exception;
        boolean ownedRecoveryRequired()throws Exception;
        boolean currentContextSupported()throws Exception;
        boolean requestCurrent(ApplyOperation.Request r,String config)throws Exception;
        boolean snapshotCurrent(ApplyOperation.Snapshot s)throws Exception;
        ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception;
        ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception;
    }
    private final Facts facts;
    ReadOnlyApplyAdapter(Facts facts){this.facts=facts;}
    public long now(){return System.nanoTime()/1000000L;}
    public boolean qualified()throws Exception{return facts.qualified();}
    public boolean disableQualified()throws Exception{return facts.disableQualified();}
    public boolean authority()throws Exception{return facts.authority();}
    public boolean ownedRecoveryRequired()throws Exception{return facts.ownedRecoveryRequired();}
    public boolean currentContextSupported()throws Exception{return facts.currentContextSupported();}
    public boolean requestCurrent(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return facts.requestCurrent(r,s==null?null:s.configIdentity);}
    public ApplyOperation.Fence fence(ApplyOperation.Snapshot s)throws Exception{return facts.snapshotCurrent(s)?ApplyOperation.Fence.SAME:ApplyOperation.Fence.UNKNOWN;}
    public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return facts.observe(r,s);}
    public ApplyOperation.DisabledReceipt observeDisabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return facts.disabled(r,s);}
    public ApplyOperation.Snapshot capture(ApplyOperation.Request r){throw unavailable();}
    public void armRecovery(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw unavailable();}
    public void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw unavailable();}
    public void disable(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw unavailable();}
    public void restore(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean play,long d){throw unavailable();}
    public void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean wait,long d){throw unavailable();}
    public boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean restore,long d){throw unavailable();}
    public void pause(long millis){throw unavailable();}
    private static UnsupportedOperationException unavailable(){return new UnsupportedOperationException("This fixed adapter can only read preparation/status facts");}
}
