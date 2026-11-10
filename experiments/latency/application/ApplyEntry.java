// SPDX-License-Identifier: GPL-2.0-only
import java.util.concurrent.atomic.AtomicBoolean;

/** Service-free dispatch policy; the resident Android branch is fixed and asynchronous. */
final class ApplyEntry {
    interface Completion {void finished(ApplyCommand.Output out,Exception failure);}
    interface Factory {
        boolean enabled();
        ApplyOperation.Adapter readOnly(String operationId)throws Exception;
        void worker(ApplyInvocation invocation,ApplyCommand.Environment environment,Completion completion)throws Exception;
        void loop()throws Exception;
    }
    static final Factory UNQUALIFIED=new Factory() {
        public boolean enabled(){return false;}
        public ApplyOperation.Adapter readOnly(String id){throw new IllegalStateException("The factory is not qualified");}
        public void worker(ApplyInvocation i,ApplyCommand.Environment e,Completion c){throw new IllegalStateException("The resident factory is not qualified");}
        public void loop(){throw new IllegalStateException("No resident event loop was started");}
    };
    /** True means the one process must enter its Android loop to await completion. */
    static boolean start(String[] arguments,ApplyCommand.Environment environment,Factory factory,Completion completion) {
        AtomicBoolean finished=new AtomicBoolean();Completion once=(out,failure)->{
            if(finished.compareAndSet(false,true))completion.finished(out,failure!=null?failure:out==null?new IllegalStateException("No final application result"):null);
        };
        try {
            ApplyInvocation invocation=ApplyInvocation.parse(arguments);environment.authority();
            if(!factory.enabled()) {
                // A stale launch must not bind/reclassify a queued owner while
                // this package's resident executor is unqualified.
                if(invocation.kind==ApplyInvocation.Kind.WORKER)throw new IllegalStateException("No qualified resident application executor");
                once.finished(ApplyCommand.run(invocation.arguments(),environment,new UnqualifiedApplyAdapter()),null);
            } else if(invocation.kind==ApplyInvocation.Kind.WORKER) {
                workerReady(invocation,environment);factory.worker(invocation,environment,once);
            } else {
                String selected=invocation.id;
                if(invocation.kind==ApplyInvocation.Kind.STATUS&&selected==null)selected=environment.store().currentId();
                once.finished(ApplyCommand.run(invocation.arguments(),environment,factory.readOnly(selected)),null);
            }
        }catch(Exception failure){once.finished(null,failure);}
        return !finished.get();
    }
    static ApplyStore.Record workerReady(ApplyInvocation invocation,ApplyCommand.Environment environment)throws Exception {
        if(invocation.kind!=ApplyInvocation.Kind.WORKER)throw new IllegalArgumentException("Resident worker command required");
        environment.authority();environment.workerIdentity(invocation.guardian,invocation.guardianStart);
        ApplyStore store=environment.store();ApplyStore.Record record=store.load(invocation.id);long now=environment.now();
        if(record==null||!invocation.id.equals(store.currentId())||!record.boot.equals(environment.boot())||record.phase!=ApplyOperation.Phase.QUEUED||record.workerPid!=0||now<record.created||now-record.created>20000)throw new IllegalStateException("Current unbound worker queue unavailable or expired");
        return record;
    }
}
