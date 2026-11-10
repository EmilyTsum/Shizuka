// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Fixed normal-owner metadata finalizer. No audio, codec, configuration or PID writes. */
public final class LatencyApplyReconcileMain {
    private static ApplicationReconciler.Normal normal(String json)throws Exception {
        Map<String,Object> m=BoundedJson.object(json);Set<String> keys=new HashSet<>(Arrays.asList("schema","scope","stock_normal_verified","boot_id","hal_pid","hal_start","monotonic_ms","inputs_sha256"));
        if(!m.keySet().equals(keys)||!Long.valueOf(1).equals(m.get("schema"))||!"application_stock_normal".equals(m.get("scope"))||!Boolean.TRUE.equals(m.get("stock_normal_verified")))throw new IOException("Independent normal-owner evidence unavailable");
        ApplicationReconciler.Normal n=new ApplicationReconciler.Normal();n.boot=text(m,"boot_id",ApplicationInputs.UUID);n.start=text(m,"hal_start","[1-9][0-9]{0,18}");n.inputsHash=text(m,"inputs_sha256",ApplicationInputs.HEX);
        n.hal=(int)number(m,"hal_pid",Integer.MAX_VALUE);n.observedAt=number(m,"monotonic_ms",Long.MAX_VALUE);n.verified=true;return n;
    }
    private static String text(Map<String,Object> m,String key,String regex)throws IOException {Object v=m.get(key);if(!(v instanceof String)||!((String)v).matches(regex))throw new IOException("Invalid normal proof text");return(String)v;}
    private static long number(Map<String,Object> m,String key,long maximum)throws IOException {Object v=m.get(key);if(!(v instanceof Long)||(Long)v<0||(Long)v>maximum)throw new IOException("Invalid normal proof integer");return(Long)v;}
    private static void current(InstalledRootSource source,String boot,String pkg,ApplicationReconciler.Normal normal)throws Exception {
        source.requireNormalRecoveryLock();
        if(!boot.equals(source.boot())||!pkg.equals(source.packageHash())||source.hal()!=normal.hal||!normal.start.equals(source.start(normal.hal))||!normal.inputsHash.equals(source.hash("inputs.sha256"))||source.now()<normal.observedAt||source.now()-normal.observedAt>=2000)throw new IOException("Normal owner identity changed before reconciliation");
        for(String marker:new String[]{"state/latency-runtime.active","state/latency-remote-uncertain","state/latency-provider.pid","state/latency-provider.start"})if(source.exists(marker))throw new IOException("Runtime recovery remains pending");
    }
    static Map<String,Object> reconcile(Path module)throws Exception {
        InstalledRootSource source=new InstalledRootSource(module,0,null);source.requireNormalRecoveryLock();
        String boot=source.boot(),pkg=source.packageHash();Map<String,Object> first=LatencyApplyFenceMain.inspect(module);
        if(!Boolean.TRUE.equals(first.get("known")))throw new IOException("Unknown application metadata cannot be cleared");
        if(Boolean.TRUE.equals(first.get("normal_mutation_allowed")))return result(boot,0);
        Path directory=module.resolve("state/latency-application");ApplyStore store=new ApplyStore(directory,0);List<ApplicationReconciler.Bundle> bundles=new ArrayList<>();
        try(DirectoryStream<Path> files=Files.newDirectoryStream(directory,"*.bin")) {
            for(Path path:files) {
                String name=path.getFileName().toString();if(!name.matches(ApplicationInputs.UUID+"\\.bin"))throw new IOException("Unknown application journal filename");
                ApplyStore.Record record=store.load(name.substring(0,36));if(!record.unresolved())continue;
                ApplicationReconciler.Bundle b=new ApplicationReconciler.Bundle();b.record=record;String prefix="state/latency-application/channel."+record.request.id+"/";
                boolean channel=source.exists("state/latency-application/channel."+record.request.id);
                if(channel&&source.exists(prefix+"capture.bin")){b.captureBytes=source.file(prefix+"capture.bin",8192,true);b.capture=ApplicationCapture.decode(b.captureBytes);}
                if(channel&&source.exists(prefix+"runtime.identity"))b.runtimeIdentity=source.file(prefix+"runtime.identity",230,true);
                String owner="state/latency-application/owned."+record.request.id;if(source.exists(owner))b.owner=ApplicationOwnerRecord.decode(source.file(owner,1024,true));
                bundles.add(b);if(bundles.size()>ApplyStore.MAX_OPERATIONS)throw new IOException("Too many unresolved records");
            }
        }
        ApplicationReconciler.Normal normal=normal(source.stockNormal());current(source,boot,pkg,normal);
        List<ApplicationReconciler.Resolution> resolutions=ApplicationReconciler.verify(bundles,boot,pkg,source.now(),normal,source::start);
        Path request=module.resolve("state/latency-runtime.request");
        if(source.exists("state/latency-runtime.request")) {
            byte[] bytes=source.file("state/latency-runtime.request",256,true);ApplicationReconciler.Resolution owner=ApplicationReconciler.requestOwner(bytes,resolutions);
            Path archive=directory.resolve("channel."+owner.bundle.record.request.id+"/request.cancelled");PrivateApplicationFiles.publishOnce(archive,bytes);
            current(source,boot,pkg,normal);
            if(!Arrays.equals(bytes,source.file("state/latency-runtime.request",256,true)))throw new IOException("Pending request changed before archive");
            Files.delete(request);PrivateApplicationFiles.sync(module.resolve("state"));
        }
        for(ApplicationReconciler.Resolution resolution:resolutions) {
            current(source,boot,pkg,normal);ApplyStore.Record record=resolution.bundle.record;Path receipt=directory.resolve("reconciled."+record.request.id);
            if(Files.exists(receipt,LinkOption.NOFOLLOW_LINKS)) {
                Map<String,Object> old=BoundedJson.object(ApplicationInputs.utf8(LatencyApplyFenceMain.privateRead(receipt,4096)));
                if(!ApplicationReconciler.receiptMatches(old,record))throw new IOException("Prior reconciliation receipt mismatch");
            }else PrivateApplicationFiles.publishOnce(receipt,(ControlJson.encode(ApplicationReconciler.receipt(resolution,boot,pkg,normal))+"\n").getBytes(StandardCharsets.UTF_8));
            current(source,boot,pkg,normal);
            store.resolveVerifiedOrphan(record.request.id,boot,"Verified normal-owner orphan recovery",source.now());
        }
        Map<String,Object> after=LatencyApplyFenceMain.inspect(module);if(!Boolean.TRUE.equals(after.get("normal_mutation_allowed")))throw new IOException("Application ownership remains unresolved");
        return result(boot,resolutions.size());
    }
    private static Map<String,Object> result(String boot,int count){return ControlJson.object("schema",1,"scope","latency_application_reconcile","resolved",true,"unresolved",false,"current_boot_id",boot,"reconciled_count",count,"reason","Application metadata is resolved; no playback or configuration was changed");}
    public static void main(String[] args) {
        try{if(args.length!=2||!args[0].equals("--module-dir"))throw new IllegalArgumentException("Fixed installed module required");System.out.println(ControlJson.encode(reconcile(Paths.get(args[1]))));}
        catch(Exception unavailable){System.out.println(ControlJson.encode(ControlJson.object("schema",1,"scope","latency_application_reconcile","resolved",false,"unresolved",true,"reason","Independent normal-owner recovery has not been verified")));System.err.println("Application reconciliation unavailable: "+unavailable.getClass().getSimpleName());System.exit(3);}
    }
}
