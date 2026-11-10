// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.*;

/** Read-only authentication for one fixed inherited-lock action, request or health hook. */
public final class LatencyOwnedGuardMain {
    public static void main(String[] args) {
        try {
            if(args.length!=13||!args[0].equals("--module-dir")||!args[2].matches("action|request|health"))throw new IllegalArgumentException("Fixed guard mode and ten action fields required");
            String mode=args[2];int action=(int)ApplicationInputs.decimal(args[3],5);String id=args[4];int hq=(int)ApplicationInputs.decimal(args[5],150),ll=(int)ApplicationInputs.decimal(args[6],100);long deadline=ApplicationInputs.decimal(args[7],Long.MAX_VALUE);
            String token=args[8],config=args[9];boolean flag;if(args[10].equals("0"))flag=false;else if(args[10].equals("1"))flag=true;else throw new IllegalArgumentException("Invalid flag");
            int pid=(int)ApplicationInputs.decimal(args[11],Integer.MAX_VALUE);String ticks=args[12];ApplicationInputs.decimal(ticks,Long.MAX_VALUE);
            if(action==0||!id.matches(ApplicationInputs.UUID)||!token.matches(ApplicationInputs.HEX)||!config.matches(ApplicationInputs.HEX)||action!=4&&flag)throw new IllegalArgumentException("Invalid fixed guard fields");
            if(mode.equals("request")&&(action!=2||flag)||mode.equals("health")&&(action!=2&&action!=3&&action!=5||flag))throw new IllegalArgumentException("Action is not allowed for this hook");
            Path module=Paths.get(args[1]);InstalledRootSource source=new InstalledRootSource(module,pid,ticks);source.requireInheritedGuardian(id,pid,ticks,action==5);
            if(deadline<=source.now()||deadline-source.now()>210000)throw new IllegalArgumentException("Owned deadline expired/outside bound");
            ApplyStore store=new ApplyStore(module.resolve("state/latency-application"),0);ApplyStore.Record journal=store.load(id);
            if(journal==null||journal.workerPid!=pid||!journal.workerStart.equals(ticks)||!journal.request.id.equals(id)||journal.request.hqMs!=hq||journal.request.llMs!=ll||!journal.boot.equals(source.boot()))throw new IllegalArgumentException("Current journal/guardian mismatch");
            byte[] bytes=source.file("state/latency-application/channel."+id+"/capture.bin",8192,true);ApplicationCapture capture=ApplicationCapture.decode(bytes);
            if(!capture.request.id.equals(id)||!capture.request.sameValues(journal.request)||!capture.snapshot.token.equals(token)||!capture.snapshot.configIdentity.equals(config)||!capture.snapshot.epoch.boot.equals(source.boot()))throw new IllegalArgumentException("Immutable snapshot mismatch");
            if(action!=5&&!new RootFacts(source).requestCurrent(capture.request,config))throw new IllegalArgumentException("Saved request changed");
            if(action==1) {
                if(journal.phase!=ApplyOperation.Phase.PREFLIGHT)throw new IllegalArgumentException("ARM is outside preflight");
            } else {
                ApplicationOwnerRecord owner=ApplicationOwnerRecord.decode(source.file("state/latency-application/owned."+id,1024,true));
                if(!owner.id.equals(id)||!owner.boot.equals(source.boot())||!owner.token.equals(token)||!owner.config.equals(config)||owner.hq!=hq||owner.ll!=ll||owner.guardian!=pid||!owner.guardianStart.equals(ticks)||
                    !owner.captureHash.equals(ApplicationInputs.sha(bytes))||!owner.packageHash.equals(source.packageHash())||owner.oldHal!=capture.snapshot.epoch.halPid||!owner.oldStart.equals(capture.snapshot.epoch.halStart))throw new IllegalArgumentException("Armed owner receipt mismatch");
                if((action==2||action==3)&&(journal.phase!=ApplyOperation.Phase.RECONFIGURING||!owner.phase.equals("armed")))throw new IllegalArgumentException("Recreation has no current armed owner");
                if(mode.equals("action")&&(action==2||action==3)&&(source.hal()!=capture.snapshot.epoch.halPid||!capture.snapshot.epoch.halStart.equals(source.start(capture.snapshot.epoch.halPid))||!owner.activationHash.equals(source.activationHash())))throw new IllegalArgumentException("Original session changed before recreation");
                if(action==4&&journal.phase!=ApplyOperation.Phase.RESTORING&&journal.phase!=ApplyOperation.Phase.VERIFYING)throw new IllegalArgumentException("FINISH is outside restoration/verification");
                if(action==5&&ApplyOperation.terminal(journal.phase))throw new IllegalArgumentException("Resident recovery cannot take a terminal journal");
            }
            if(mode.equals("request")&&(source.exists("state/latency-runtime.active")||source.exists("state/latency-remote-uncertain")))throw new IllegalArgumentException("Old runtime ownership is unresolved");
            if(mode.equals("health")) {
                RootFacts facts=new RootFacts(source);RootFacts.Context context=facts.inspect();
                if(!context.authority||context.recovery||(action==2?!ApplicationOwnerWriter.healthyNew(context,capture.snapshot):!facts.normalVerified(context,capture.snapshot)))throw new IllegalArgumentException("Full new owner readiness is not verified");
            }
            source.requireInheritedGuardian(id,pid,ticks,action==5);if(deadline<=source.now())throw new IllegalArgumentException("Owned guard deadline expired");
        }catch(Exception rejected){System.err.println("Owned action authentication unavailable: "+rejected.getClass().getSimpleName());System.exit(20);}
    }
}
