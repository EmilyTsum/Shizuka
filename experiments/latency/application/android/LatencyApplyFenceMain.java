// SPDX-License-Identifier: GPL-2.0-only
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Fixed read-only check/health entry. It never clears a pending journal. */
public final class LatencyApplyFenceMain {
    static byte[] privateRead(Path path,int maximum)throws Exception {
        String before=InstalledRootSource.stamp(path,true);byte[] bytes=InstalledRootSource.read(path,maximum);
        if(!before.equals(InstalledRootSource.stamp(path,true)))throw new java.io.IOException("Private fence input changed");return bytes;
    }
    private static boolean actor(Path directory,ApplyStore.Record r)throws Exception {
        if(!r.workerStart.equals(InstalledRootSource.processStart(r.workerPid))||!InstalledRootSource.rootProcessStatus(r.workerPid))return false;
        Path identity=directory.resolve("channel."+r.request.id+"/worker.identity");
        if(!Files.exists(identity,LinkOption.NOFOLLOW_LINKS)) {
            long age=System.nanoTime()/1000000L-r.created;
            return age>=0&&age<=ApplicationFence.QUEUE_GRACE&&(r.phase==ApplyOperation.Phase.QUEUED||r.phase==ApplyOperation.Phase.PREFLIGHT);
        }
        String value=new String(privateRead(identity,128),StandardCharsets.US_ASCII);
        if(!value.matches("[1-9][0-9]{0,9} [1-9][0-9]{0,18} "+ApplicationInputs.UUID+"\n"))return false;
        String[] f=value.trim().split(" ");int pid=(int)ApplicationInputs.decimal(f[0],Integer.MAX_VALUE);
        if(!r.boot.equals(f[2])||!f[1].equals(InstalledRootSource.processStart(pid))||!InstalledRootSource.rootProcessStatus(pid))return false;
        String status=new String(InstalledRootSource.read(Paths.get("/proc/"+pid+"/status"),16384),StandardCharsets.US_ASCII);
        for(String line:status.split("\n"))if(line.startsWith("PPid:"))return line.substring(5).trim().equals(""+r.workerPid);
        return false;
    }
    static Map<String,Object> inspect(Path module)throws Exception {
        if(!module.toString().equals("/data/adb/modules/s25_aptx_adaptive")||!module.equals(module.toRealPath()))throw new java.io.IOException("Noncanonical module");
        InstalledRootSource.directory(module,0,false);InstalledRootSource.directory(module.resolve("state"),0,true);
        Path properties=module.resolve("module.prop");String propertyStamp=InstalledRootSource.stamp(properties,false);String text=ApplicationInputs.utf8(InstalledRootSource.read(properties,4096));
        int ids=0;for(String line:text.split("\n"))if(line.equals("id=s25_aptx_adaptive"))ids++;if(ids!=1||!propertyStamp.equals(InstalledRootSource.stamp(properties,false)))throw new java.io.IOException("Module identity mismatch");
        if(!InstalledRootSource.rootStatus(new String(InstalledRootSource.read(Paths.get("/proc/self/status"),16384),StandardCharsets.US_ASCII)))throw new java.io.IOException("Current root authority unavailable");
        String boot=new String(InstalledRootSource.read(Paths.get("/proc/sys/kernel/random/boot_id"),64),StandardCharsets.US_ASCII).trim();if(!boot.matches(ApplicationInputs.UUID))throw new java.io.IOException("Invalid boot");
        Path directory=module.resolve("state/latency-application");
        if(!Files.exists(directory,LinkOption.NOFOLLOW_LINKS))return ApplicationFence.evaluate(Collections.emptyList(),null,boot,System.nanoTime()/1000000L,true,r->false);
        InstalledRootSource.directory(directory,0,true);ApplyStore store=new ApplyStore(directory,0);List<ApplyStore.Record> records=new ArrayList<>();Map<String,ApplyStore.Record> byId=new HashMap<>();boolean known=true;
        try(DirectoryStream<Path> entries=Files.newDirectoryStream(directory)) {
            for(Path path:entries) {
                String name=path.getFileName().toString();
                if(name.matches(ApplicationInputs.UUID+"\\.bin")){String id=name.substring(0,36);ApplyStore.Record r=store.load(id);records.add(r);byId.put(id,r);if(records.size()>ApplyStore.MAX_OPERATIONS)throw new java.io.IOException("Too many records");}
                else if(name.equals("current"))privateRead(path,64);
                else if(name.startsWith(".apply-")||name.startsWith(".owned-")){privateRead(path,ApplyStore.MAX_RECORD);}
                else if(name.matches("channel\\."+ApplicationInputs.UUID))InstalledRootSource.directory(path,0,true);
                else if(!name.matches("(owned|reconciled)\\."+ApplicationInputs.UUID))known=false;
            }
        }
        String current=store.currentId();if(current!=null&&!byId.containsKey(current)){ApplyStore.Record r=store.load(current);if(r==null)known=false;else {records.add(r);byId.put(current,r);}}
        try(DirectoryStream<Path> entries=Files.newDirectoryStream(directory,"owned.*")) {
            for(Path path:entries) {
                ApplicationOwnerRecord o=ApplicationOwnerRecord.decode(privateRead(path,1024));ApplyStore.Record r=byId.get(o.id);
                if(!path.getFileName().toString().equals("owned."+o.id)||r==null||!r.boot.equals(o.boot)||r.request.hqMs!=o.hq||r.request.llMs!=o.ll)known=false;
                byte[] capture=privateRead(directory.resolve("channel."+o.id+"/capture.bin"),8192);ApplicationCapture decoded=ApplicationCapture.decode(capture);
                if(!o.captureHash.equals(ApplicationInputs.sha(capture))||!decoded.request.id.equals(o.id)||!decoded.snapshot.token.equals(o.token)||!decoded.snapshot.configIdentity.equals(o.config))known=false;
            }
        }
        try(DirectoryStream<Path> entries=Files.newDirectoryStream(directory,"reconciled.*")) {
            for(Path path:entries) {
                String name=path.getFileName().toString();String id=name.substring("reconciled.".length());ApplyStore.Record r=byId.get(id);
                if(r==null||!ApplicationReconciler.receiptMatches(BoundedJson.object(ApplicationInputs.utf8(privateRead(path,4096))),r))known=false;
            }
        }
        return ApplicationFence.evaluate(records,current,boot,System.nanoTime()/1000000L,known,r->actor(directory,r));
    }
    public static void main(String[] args) {
        String mode=args.length==3?args[2]:"";Map<String,Object> out;
        try{if(args.length!=3||!args[0].equals("--module-dir")||!mode.matches("check|health"))throw new IllegalArgumentException("Fixed check|health required");out=inspect(Paths.get(args[1]));}
        catch(Exception unknown){out=ControlJson.object("schema",1,"scope","latency_application_fence","known",false,"unresolved",true,"normal_mutation_allowed",false,"healthy",false,"orphaned",true,"reason","Application metadata needs verified owner recovery");}
        System.out.println(ControlJson.encode(out));System.exit(Boolean.TRUE.equals(out.get(mode.equals("health")?"healthy":"normal_mutation_allowed"))?0:3);
    }
}
