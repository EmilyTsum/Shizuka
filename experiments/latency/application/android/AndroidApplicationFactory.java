// SPDX-License-Identifier: GPL-2.0-only
import android.os.Looper;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicReference;

/** Fixed resident implementation; every production capability remains false. */
final class AndroidApplicationFactory implements ApplyEntry.Factory {
    private static final boolean QUALIFIED=false;
    private final Path module;
    AndroidApplicationFactory(Path module){this.module=module;}
    public boolean enabled(){return QUALIFIED;}
    private void requireQualified(){if(!QUALIFIED)throw new IllegalStateException("The packaged Android resident factory is not qualified");}
    public ApplyOperation.Adapter readOnly(String operationId)throws Exception {
        requireQualified();return new ReadOnlyApplyAdapter(new ReadOnlyRootFacts(module,operationId,0,null));
    }
    public void worker(ApplyInvocation invocation,ApplyCommand.Environment environment,ApplyEntry.Completion completion)throws Exception {
        requireQualified();ApplyStore.Record record=ApplyEntry.workerReady(invocation,environment);
        ReadOnlyRootFacts facts=new ReadOnlyRootFacts(module,invocation.id,invocation.guardian,invocation.guardianStart);
        facts.requireResidentGuardian(invocation.id,invocation.guardian,invocation.guardianStart);
        boolean disabling=record.request.hqMs==0;
        if(disabling?!facts.disableQualified():!facts.qualified())throw new IllegalStateException("This operation's lifecycle executor is not qualified");
        FixedLifecycleOwner owner=new FixedLifecycleOwner(module,invocation.id,facts);AtomicReference<ApplyCommand.Output> result=new AtomicReference<>();
        AndroidApplyBootstrap.start(owner,disabling,adapter->{
            ApplyEntry.workerReady(invocation,environment);
            facts.requireResidentGuardian(invocation.id,invocation.guardian,invocation.guardianStart);
            result.set(ApplyCommand.run(invocation.arguments(),environment,adapter));
        },failure->completion.finished(result.get(),failure));
    }
    public void loop() {
        requireQualified();
        // Completion closes local resources and exits this app_process. Android's
        // main Looper is not quittable; never call quit/quitSafely here.
        Looper.loop();throw new IllegalStateException("Resident loop ended without a final result");
    }
}
