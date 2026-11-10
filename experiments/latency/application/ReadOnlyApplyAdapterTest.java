// SPDX-License-Identifier: GPL-2.0-only
public final class ReadOnlyApplyAdapterTest {
    static int checks;static void check(boolean v){checks++;if(!v)throw new AssertionError("read-only adapter check "+checks);}
    interface Task{void run()throws Exception;}static void rejects(Task task)throws Exception{try{task.run();throw new AssertionError("Read-only mutation accepted");}catch(UnsupportedOperationException expected){checks++;}}
    static final class Facts implements ReadOnlyApplyAdapter.Facts {
        boolean current=true;String hash;int observations;
        public boolean qualified(){return false;}public boolean disableQualified(){return false;}public boolean authority(){return true;}public boolean ownedRecoveryRequired(){return false;}public boolean currentContextSupported(){return true;}
        public boolean requestCurrent(ApplyOperation.Request r,String c){hash=c;return current;}public boolean snapshotCurrent(ApplyOperation.Snapshot s){return current;}
        public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s){observations++;return null;}public ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s){observations++;return null;}
    }
    public static void main(String[] args)throws Exception {
        Facts f=new Facts();ReadOnlyApplyAdapter a=new ReadOnlyApplyAdapter(f);ApplyOperation.Request r=new ApplyOperation.Request(ApplyOperationTest.ID,150,0);ApplyOperation.Snapshot s=ApplyOperationTest.snapshot(true,44100,44100);
        check(!a.qualified()&&!a.disableQualified()&&a.authority()&&!a.ownedRecoveryRequired()&&a.currentContextSupported());
        check(a.requestCurrent(r,null)&&f.hash==null);check(a.requestCurrent(r,s)&&f.hash.equals(s.configIdentity));
        check(a.fence(s)==ApplyOperation.Fence.SAME);f.current=false;check(a.fence(s)==ApplyOperation.Fence.UNKNOWN);
        check(a.observe(r,s)==null&&a.observeDisabled(r,s)==null&&f.observations==2);
        rejects(()->a.capture(r));rejects(()->a.armRecovery(r,s,0));rejects(()->a.recreate(r,s,0));rejects(()->a.disable(r,s,0));rejects(()->a.restore(r,s,true,0));rejects(()->a.finish(r,s,false,0));rejects(()->a.recover(r,s,true,0));rejects(()->a.pause(0));
        System.out.println("Read-only apply adapter: "+checks+" evidence delegation/no media/no mutation checks passed.");
    }
}
