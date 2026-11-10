// SPDX-License-Identifier: GPL-2.0-only
/** Shipped preparation deliberately has no device lifecycle executor. */
final class UnqualifiedApplyAdapter implements ApplyOperation.Adapter {
    public long now(){return System.nanoTime()/1000000L;}
    public boolean qualified(){return false;}
    public boolean authority(){return false;}
    public boolean requestCurrent(ApplyOperation.Request r,ApplyOperation.Snapshot s){return false;}
    public ApplyOperation.Snapshot capture(ApplyOperation.Request r){throw unavailable();}
    public ApplyOperation.Fence fence(ApplyOperation.Snapshot s){return ApplyOperation.Fence.UNKNOWN;}
    public void armRecovery(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw unavailable();}
    public void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){throw unavailable();}
    public void restore(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean p,long d){throw unavailable();}
    public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s){return null;}
    public void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean w,long d){throw unavailable();}
    public boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean restore,long d){return false;}
    public void pause(long millis)throws InterruptedException{Thread.sleep(millis);}
    private static UnsupportedOperationException unavailable(){return new UnsupportedOperationException("No qualified lifecycle executor");}
}
