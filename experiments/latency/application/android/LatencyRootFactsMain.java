// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.*;
import java.util.Map;

/** Fixed read-only facts CLI for owner tooling; not an enabling capability. */
public final class LatencyRootFactsMain {
    public static void main(String[] args) {
        try {
            if(args.length<3||!args[0].equals("--module-dir")||!args[2].matches("inspect|observe|disabled")||
                (args[2].equals("inspect")?args.length!=3:args.length!=4||!args[3].matches(ApplicationInputs.UUID)))throw new IllegalArgumentException("--module-dir INSTALLED_PATH inspect|observe UUID|disabled UUID");
            InstalledRootSource source=new InstalledRootSource(Paths.get(args[1]),0,null);RootFacts facts=new RootFacts(source);RootFacts.Context c=facts.inspect();
            Map<String,Object> out=ControlJson.object("schema",1,"scope","application_root_facts","available",false,"qualified_executor",false,
                "root_authority",c.authority,"owned_recovery_required",c.recovery,"boot_id",c.epoch.boot,"cookie",c.epoch.cookie,"hal_pid",c.epoch.halPid,"hal_start",c.epoch.halStart,
                "graph_epoch",Long.toString(c.epoch.graph),"config_sha256",c.config.hash,"requested_hq_ms",c.config.hq,"requested_ll_ms",c.config.ll,
                "package_sha256",c.packageHash,"activation_sha256",c.activationHash,"profile_observed",c.reviewedHq,"target_eligible",c.targetEligible,"sample_rate_hz",c.observedRate,
                "selected_rate_hz",c.selectedRate,"selection_known",c.selectionKnown,"normal_removal_preflight",c.normalRemovalPossible);
            if(!args[2].equals("inspect")) {
                ApplicationCapture capture=ApplicationCapture.decode(source.file("state/latency-application/channel."+args[3]+"/capture.bin",8192,true));
                if(!capture.request.id.equals(args[3]))throw new IllegalArgumentException("Capture operation mismatch");
                out.put("operation_id",args[3]);out.put("snapshot_token",capture.snapshot.token);
                if(args[2].equals("observe")) {
                    ApplyOperation.Proof p=facts.observe(capture.request,capture.snapshot);boolean valid=p!=null&&p.applied(capture.request,capture.snapshot,source.now());out.put("observed_hq_proof",valid);
                    if(valid){out.put("graph_epoch",Long.toString(p.epoch.graph));out.put("active_delta_us",p.deltaUs);out.put("wr_frames",p.wr);out.put("confirmed_rewritten_wrappers",p.confirmed);}
                } else {ApplyOperation.DisabledReceipt receipt=facts.disabled(capture.request,capture.snapshot);out.put("normal_removal_receipt_valid",receipt!=null&&receipt.valid(capture.request,capture.snapshot,source.now()));}
            }
            System.out.println(ControlJson.encode(out));
        }catch(Exception unavailable){System.out.println(ControlJson.encode(ControlJson.object("schema",1,"scope","application_root_facts","available",false,"qualified_executor",false,"root_authority",false,"owned_recovery_required",true,"reason","Fixed read-only facts unavailable: "+unavailable.getClass().getSimpleName())));}
    }
}
