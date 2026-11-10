// SPDX-License-Identifier: GPL-2.0-only
import java.util.Map;

/** Literal CLI policy; the shell owns the module lifecycle flock around mutations. */
final class ApplyCommand {
    static final int QUEUED=10;
    interface Environment {
        ApplyStore store() throws Exception;
        String boot() throws Exception;
        long now();
        void authority() throws Exception;
        void workerIdentity(int pid,String start) throws Exception;
    }
    static final class Output {
        final int code;final Map<String,Object> json;
        Output(int code,Map<String,Object> json){this.code=code;this.json=json;}
    }
    private static boolean id(String value){return value!=null && value.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");}
    private static int number(String value,int maximum) {
        if(value==null || !value.matches("0|[1-9][0-9]{0,2}"))throw new IllegalArgumentException("Noncanonical delay argument");
        int number=Integer.parseInt(value);if(number>maximum)throw new IllegalArgumentException("Delay argument exceeds domain");return number;
    }
    static Output run(String[] args,Environment env,ApplyOperation.Adapter adapter) throws Exception {
        ApplyInvocation.parse(args);
        Output out=execute(args,env,adapter);out.json.put("current_boot_id",env.boot());return out;
    }
    private static Output execute(String[] args,Environment env,ApplyOperation.Adapter adapter) throws Exception {
        if(args.length==0)throw new IllegalArgumentException("Missing application command");
        env.authority();String command=args[0];
        if(command.equals("status")) {
            if(args.length>2 || args.length==2&&!id(args[1]))throw new IllegalArgumentException("status [UUID]");
            ApplyStore store=env.store();String current=store.currentId(), selected=args.length==2?args[1]:current;
            Map<String,Object> out=status(selected==null?null:store.load(selected),selected,current,adapter,env.now());
            String latest=store.currentId();out.put("current_operation_id",latest);
            if(!java.util.Objects.equals(current,latest)) {
                out.put("applied",false);out.put("disabled",false);out.put("disable_receipt_valid",false);out.put("phase","superseded");out.put("terminal",true);out.put("pending_hq",true);out.put("retry_safe",false);
                out.put("reason","A newer operation took ownership during the status read");
            }
            return new Output(0,out);
        }
        if(command.equals("prepare")) {
            if(args.length!=4)throw new IllegalArgumentException("prepare UUID HQ_MS LL_MS");
            ApplyOperation.Request request=new ApplyOperation.Request(args[1],number(args[2],150),number(args[3],100));
            boolean qualified=request.hqMs==0?adapter.disableQualified():adapter.qualified();
            boolean supported=request.hqMs==0 || adapter.currentContextSupported();
            if(adapter.ownedRecoveryRequired()) {
                Map<String,Object> out=ApplyStatus.view(null,request.id,null,false,false,false,false,null,null,env.now());
                out.put("phase","unconfirmed");out.put("owned_recovery_required",true);out.put("retry_safe",false);
                out.put("requested_hq_ms",request.hqMs);out.put("requested_ll_ms",request.llMs);
                out.put("reason","The lifecycle owner must complete verified remote recovery before another application");
                return new Output(0,out);
            }
            if(!qualified || !supported) {
                Map<String,Object> out=ApplyStatus.view(null,request.id,null,false,false,false,false,null,null,env.now());
                out.put("phase","rejected");out.put("requested_hq_ms",request.hqMs);out.put("requested_ll_ms",request.llMs);
                out.put("reason",!qualified?"The packaged lifecycle executor is not qualified; saved settings remain pending":
                    "The actual selected session is not eligible for the reviewed Adaptive HQ 44.1 kHz attempt; saved settings remain pending");
                return new Output(0,out);
            }
            if(!adapter.authority() || !adapter.requestCurrent(request,null))throw new IllegalStateException("Current saved request or authority changed");
            ApplyStore store=env.store();ApplyStore.Claim claim=store.claim(request,env.boot(),env.now());
            if(claim==ApplyStore.Claim.CONFLICT)throw new IllegalArgumentException("Operation UUID already has different values");
            if(claim==ApplyStore.Claim.BUSY || claim==ApplyStore.Claim.FULL) {
                Map<String,Object> out=status(store.current(),store.currentId(),store.currentId(),adapter,env.now());
                out.put("accepted",false);out.put("reason",claim==ApplyStore.Claim.BUSY?"An unresolved application already owns the session":"The bounded application journal is full");
                return new Output(0,out);
            }
            return new Output(claim==ApplyStore.Claim.NEW?QUEUED:0,status(store.load(request.id),request.id,store.currentId(),adapter,env.now()));
        }
        if(command.equals("worker")) {
            if(args.length!=4 || !id(args[1]) || !args[2].matches("[1-9][0-9]{0,9}") || !args[3].matches("[1-9][0-9]{0,23}"))
                throw new IllegalArgumentException("worker UUID GUARDIAN_PID START_TICKS");
            int pid=Integer.parseInt(args[2]);env.workerIdentity(pid,args[3]);
            ApplyStore store=env.store();ApplyStore.Record record=store.load(args[1]);
            if(record==null || !record.boot.equals(env.boot()))throw new IllegalStateException("Worker journal or boot mismatch");
            // A delayed launch never revives a recovered/rejected operation.
            if(record.phase!=ApplyOperation.Phase.QUEUED || env.now()<record.created || env.now()-record.created>20000)
                throw new IllegalStateException("Worker queue expired or operation already started");
            store.bind(args[1],pid,args[3],env.now());
            ApplyOperation.run(record.request,adapter,(request,phase,snapshot,reason)->
                store.publish(request.id,phase,snapshot,reason,env.now()));
            return new Output(0,status(store.load(args[1]),args[1],store.currentId(),adapter,env.now()));
        }
        throw new IllegalArgumentException("Unknown application command");
    }
    private static Map<String,Object> status(ApplyStore.Record record,String id,String current,
            ApplyOperation.Adapter adapter,long now) throws Exception {
        boolean recovery=adapter.ownedRecoveryRequired();
        boolean available=adapter.qualified()&&!recovery,disable=adapter.disableQualified()&&!recovery,matches=false,supported=adapter.currentContextSupported();
        ApplyOperation.Proof proof=null;ApplyOperation.DisabledReceipt receipt=null;
        if(record!=null && adapter.authority()) {
            matches=adapter.requestCurrent(record.request,record.snapshot);
            if(matches && record.snapshot!=null) {
                // The adapter must re-verify process/session/config binding on every read.
                if(adapter.fence(record.snapshot)==ApplyOperation.Fence.SAME) {
                    supported=supported && record.snapshot.supported();
                    if(record.request.hqMs>0)proof=adapter.observe(record.request,record.snapshot);
                    else receipt=adapter.observeDisabled(record.request,record.snapshot);
                }
            }
        }
        Map<String,Object> out=ApplyStatus.view(record,id,current,available,supported,disable,matches,proof,receipt,now);
        if(!available&&!disable)out.put("retry_safe",false);
        out.put("owned_recovery_required",recovery);
        if(recovery){out.put("retry_safe",false);out.put("reason","The lifecycle owner must complete verified remote recovery");}
        return out;
    }
}
