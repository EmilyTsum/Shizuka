// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.TimeUnit;

/** Actual fixed installed paths/commands. No arbitrary program, config path or shell text input. */
final class InstalledRootSource implements RootFacts.Source {
    private static final String INSTALLED="/data/adb/modules/s25_aptx_adaptive";
    private static final String[] PACKAGE={"module.prop","device-profile","inputs.sha256","latency-runtime.identity","control.sh","health.sh","config.sh","status.sh","latency-runtime.sh","latency-apply.sh","latency-apply-guardian.sh","latency-apply-executor.sh","latency-apply-owned.sh","latency-apply-read.sh","latency-apply-normal.sh","latency-apply-fence.sh","settings-bootstrap.sh","settings-bridge.sh","ui-lifecycle.sh","settings.sh","action.sh","settings.apk","settings.sha256","lib/runtime.sh","lib/device.sh","lib/config.sh","lib/properties.sh","lib/policy.sh","profiles/samsung-sm-s931z-dzif.sh","policy.dex","status.dex","policy.sha256","latency-provider","latency-provider.sha256","latency-application.dex","latency-application.sha256","payload/patched-libar-gsl.so","payload/patched-libbluetooth_jni.so","payload/patched-libsehbluetooth_audio_session_aidl.so"};
    private final Path module,state;private final Object moduleKey,stateKey;private final int guardian;private final String guardianStart;
    private final Map<String,String> artifactHashes=new LinkedHashMap<>();private final Map<String,String> stamps=new LinkedHashMap<>();
    private final String packageDigest;
    InstalledRootSource(Path module,int guardian,String guardianStart)throws Exception {
        if(!module.toString().equals(INSTALLED)||!module.equals(module.toRealPath()))throw new IOException("Only canonical installed Shizuka module is accepted");
        this.module=module;this.state=module.resolve("state");this.guardian=guardian;this.guardianStart=guardianStart;
        moduleKey=directory(module,0,false);stateKey=directory(state,0,true);
        String properties=ApplicationInputs.utf8(file("module.prop",4096,false));int ids=0;for(String line:properties.split("\n"))if(line.equals("id=s25_aptx_adaptive"))ids++;if(ids!=1)throw new IOException("Module identity mismatch");
        if(!new String(file("device-profile",128,false),StandardCharsets.US_ASCII).equals("samsung-sm-s931z-dzif\n"))throw new IOException("Unsupported module backend");
        StringBuilder binding=new StringBuilder();
        for(String name:PACKAGE){String hash=hashFile(path(name),128*1024*1024);artifactHashes.put(name,hash);stamps.put(name,stamp(path(name),false));binding.append(name).append('\0').append(hash).append('\n');}
        for(String[] pair:new String[][]{{"policy.dex","policy.sha256"},{"status.dex","policy.sha256"},{"settings.apk","settings.sha256"},{"latency-provider","latency-provider.sha256"},{"latency-application.dex","latency-application.sha256"}})
            if(!new String(file(pair[1],65,false),StandardCharsets.US_ASCII).equals(artifactHashes.get(pair[0])+"\n"))throw new IOException("Source-bound helper digest mismatch");
        packageDigest=ApplicationInputs.sha(binding.toString().getBytes(StandardCharsets.US_ASCII));
    }
    private Path path(String relative)throws IOException {
        if(relative==null||relative.startsWith("/")||relative.contains("..")||relative.indexOf('\0')>=0)throw new IOException("Invalid fixed module path");
        Path p=module.resolve(relative);Path parent=p.getParent();
        if(!p.normalize().equals(p)||!parent.toRealPath().equals(parent))throw new IOException("Noncanonical module path parent");
        for(Path current=parent;current!=null&&current.startsWith(module);current=current.getParent())directory(current,0,current.startsWith(state));
        return p;
    }
    static Object directory(Path p,int uid,boolean privateMode)throws IOException {
        BasicFileAttributes a=Files.readAttributes(p,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        Set<PosixFilePermission> mode=Files.getPosixFilePermissions(p,LinkOption.NOFOLLOW_LINKS);
        if(!a.isDirectory()||((Number)Files.getAttribute(p,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=uid||
            (privateMode?!mode.equals(PosixFilePermissions.fromString("rwx------")):
            mode.contains(PosixFilePermission.GROUP_WRITE)||mode.contains(PosixFilePermission.OTHERS_WRITE)))throw new IOException("Unsafe root directory");
        if(a.fileKey()==null)throw new IOException("Directory identity unavailable");return a.fileKey();
    }
    private void pinned()throws Exception {
        if(!moduleKey.equals(directory(module,0,false))||!stateKey.equals(directory(state,0,true)))throw new IOException("Installed module/state directory changed");
    }
    static String stamp(Path p,boolean privateFile)throws Exception {
        BasicFileAttributes a=Files.readAttributes(p,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        Set<PosixFilePermission> mode=Files.getPosixFilePermissions(p,LinkOption.NOFOLLOW_LINKS);
        if(!a.isRegularFile()||((Number)Files.getAttribute(p,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=0||((Number)Files.getAttribute(p,"unix:nlink",LinkOption.NOFOLLOW_LINKS)).intValue()!=1||
            (privateFile?!mode.equals(PosixFilePermissions.fromString("rw-------")):
            mode.contains(PosixFilePermission.GROUP_WRITE)||mode.contains(PosixFilePermission.OTHERS_WRITE)))throw new IOException("Unsafe root file");
        return a.fileKey()+":"+a.size()+":"+a.lastModifiedTime()+":"+Files.getAttribute(p,"unix:ctime",LinkOption.NOFOLLOW_LINKS);
    }
    static byte[] read(Path p,int maximum)throws Exception {
        try(InputStream input=Files.newInputStream(p,LinkOption.NOFOLLOW_LINKS);ByteArrayOutputStream out=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[1024];int n;while((n=input.read(buffer))!=-1){if(out.size()+n>maximum)throw new IOException("Fixed read exceeds bound");out.write(buffer,0,n);}return out.toByteArray();
        }
    }
    public byte[] file(String relative,int maximum,boolean privateFile)throws Exception {
        pinned();Path p=path(relative);String before=stamp(p,privateFile);byte[] bytes=read(p,maximum);
        if(!before.equals(stamp(p,privateFile)))throw new IOException("Root file changed during read");pinned();return bytes;
    }
    private static String hashFile(Path p,int maximum)throws Exception {
        String before=stamp(p,false);MessageDigest digest=MessageDigest.getInstance("SHA-256");long bytes=0;
        try(InputStream input=Files.newInputStream(p,LinkOption.NOFOLLOW_LINKS)){byte[] b=new byte[65536];int n;while((n=input.read(b))!=-1){bytes+=n;if(bytes>maximum)throw new IOException("Artifact exceeds bound");digest.update(b,0,n);}}
        if(!before.equals(stamp(p,false)))throw new IOException("Artifact changed during digest");return ApplicationInputs.hex(digest.digest());
    }
    public String hash(String relative)throws Exception {
        pinned();if(artifactHashes.containsKey(relative)){verifyArtifact(relative);return artifactHashes.get(relative);}
        return hashFile(path(relative),128*1024*1024);
    }
    private synchronized void verifyArtifact(String name)throws Exception {
        String current=stamp(path(name),false);
        if(!stamps.get(name).equals(current)) {
            // The owned mount path legitimately chmods/chcons its immutable
            // payload. Recheck bytes and secure ownership rather than treating
            // an expected label/mode change as a new source generation.
            if(!artifactHashes.get(name).equals(hashFile(path(name),128*1024*1024)))throw new IOException("Package source identity changed");
            stamps.put(name,stamp(path(name),false));
        }
    }
    public String packageHash()throws Exception {pinned();for(String name:PACKAGE)verifyArtifact(name);return packageDigest;}
    public boolean exists(String relative)throws Exception {pinned();return Files.exists(path(relative),LinkOption.NOFOLLOW_LINKS);}
    public byte[] config()throws Exception {
        Path parent=Paths.get("/data/adb/shizuka"),file=parent.resolve("config.conf");Object key=directory(parent,0,true);String before=stamp(file,true);byte[] bytes=read(file,16384);
        if(!key.equals(directory(parent,0,true))||!before.equals(stamp(file,true)))throw new IOException("Private config changed during read");return bytes;
    }
    public String boot()throws Exception {String b=new String(read(Paths.get("/proc/sys/kernel/random/boot_id"),64),StandardCharsets.US_ASCII);if(!b.matches(ApplicationInputs.UUID+"\n"))throw new IOException("Invalid current boot");return b.trim();}
    public long now(){return System.nanoTime()/1000000L;}
    private static String field(String text,String key)throws IOException {String found=null;for(String line:text.split("\n"))if(line.startsWith(key)){if(found!=null)throw new IOException("Duplicate process identity field");found=line.substring(key.length()).trim();}if(found==null)throw new IOException("Missing process identity field");return found;}
    static boolean rootStatus(String status)throws IOException {return field(status,"Uid:").matches("0\\s+0\\s+0\\s+0")&&new BigInteger(field(status,"CapEff:"),16).testBit(21);}
    static boolean rootProcessStatus(int pid)throws Exception {return pid>1&&rootStatus(new String(read(Paths.get("/proc/"+pid+"/status"),16384),StandardCharsets.US_ASCII));}
    public boolean rootProcess(int pid)throws Exception {return rootProcessStatus(pid);}
    public boolean authority()throws Exception {return authority(false);}
    private boolean authority(boolean cleanup)throws Exception {
        pinned();if(!rootStatus(new String(read(Paths.get("/proc/self/status"),16384),StandardCharsets.US_ASCII))||!cleanup&&(exists("disable")||exists("remove")||!exists("state/active")))return false;
        if(exists("state/active"))stamp(path("state/active"),false);
        return guardian==0||guardian>1&&guardianStart!=null&&guardianStart.equals(start(guardian))&&rootProcess(guardian);
    }
    public String start(int pid)throws Exception {
        return processStart(pid);
    }
    static String processStart(int pid)throws Exception {
        if(pid<=1)return null;
        try {String stat=new String(read(Paths.get("/proc/"+pid+"/stat"),8192),StandardCharsets.US_ASCII);int close=stat.lastIndexOf(") ");if(close<0)return null;String[] fields=stat.substring(close+2).trim().split("\\s+");if(fields.length<20||fields[0].equals("Z")||fields[0].equals("X")||!fields[19].matches("[1-9][0-9]{0,18}"))return null;return fields[19];}
        catch(NoSuchFileException gone){return null;}
    }
    public int hal()throws Exception {
        String value=execute(Arrays.asList("/system/bin/getprop","init.svc_debug_pid.vendor.audio-hal-aidl"),1000,64);
        if(!value.matches("[1-9][0-9]{0,9}\n"))throw new IOException("Actual HAL property unavailable");return (int)ApplicationInputs.decimal(value.trim(),Integer.MAX_VALUE);
    }
    public byte[] runtime(String name,int size)throws Exception {
        if(!(name.equals("provider.bin")&&size==128||name.equals("status.bin")&&size==256))throw new IOException("Unknown fixed runtime record");
        int group=(Integer)Class.forName("android.os.Process").getMethod("getGidForName",String.class).invoke(null,"audio");
        int uid=name.equals("provider.bin")?0:(Integer)Class.forName("android.os.Process").getMethod("getUidForName",String.class).invoke(null,"audioserver");
        if(group<0||uid<0)throw new IOException("Audio identity unavailable");Path parent=Paths.get("/data/vendor/audio/shizuka"),p=parent.resolve(name);
        BasicFileAttributes d=Files.readAttributes(parent,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);Set<PosixFilePermission> dm=Files.getPosixFilePermissions(parent,LinkOption.NOFOLLOW_LINKS);
        if(!d.isDirectory()||!dm.equals(PosixFilePermissions.fromString("rwxr-x---"))||((Number)Files.getAttribute(parent,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=0||((Number)Files.getAttribute(parent,"unix:gid",LinkOption.NOFOLLOW_LINKS)).intValue()!=group)throw new IOException("Unsafe runtime directory");
        BasicFileAttributes before=Files.readAttributes(p,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        String stamp=runtimeStamp(p,uid,group,size);byte[] bytes=read(p,size);
        if(bytes.length!=size||!stamp.equals(runtimeStamp(p,uid,group,size))||!d.fileKey().equals(Files.readAttributes(parent,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS).fileKey()))throw new IOException("Runtime record changed during read");return bytes;
    }
    private static String runtimeStamp(Path p,int uid,int gid,int size)throws Exception {
        BasicFileAttributes a=Files.readAttributes(p,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        if(!a.isRegularFile()||a.size()!=size||((Number)Files.getAttribute(p,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=uid||((Number)Files.getAttribute(p,"unix:gid",LinkOption.NOFOLLOW_LINKS)).intValue()!=gid||((Number)Files.getAttribute(p,"unix:nlink",LinkOption.NOFOLLOW_LINKS)).intValue()!=1||!Files.getPosixFilePermissions(p,LinkOption.NOFOLLOW_LINKS).equals(PosixFilePermissions.fromString("rw-r-----")))throw new IOException("Unsafe runtime record");
        return a.fileKey()+":"+a.size()+":"+a.lastModifiedTime()+":"+Files.getAttribute(p,"unix:ctime",LinkOption.NOFOLLOW_LINKS);
    }
    public String activationHash()throws Exception {
        String token=new String(file("state/health-token",64,false),StandardCharsets.US_ASCII);
        if(!token.matches(ApplicationInputs.UUID+"\n"))throw new IOException("Health generation unavailable");
        StringBuilder value=new StringBuilder();value.append(stamp(path("state/active"),false)).append('\n').append(token);
        for(String f:new String[]{"state/hal.pid","state/hal.start","state/bt.pid","state/bt.start"})value.append(new String(file(f,64,false),StandardCharsets.US_ASCII));
        return ApplicationInputs.sha(value.toString().getBytes(StandardCharsets.US_ASCII));
    }
    public String currentOperation()throws Exception {
        String value=new String(file("state/latency-application/current",64,true),StandardCharsets.US_ASCII);
        if(!value.matches(ApplicationInputs.UUID+"\n"))throw new IOException("Application owner pointer unavailable");return value.trim();
    }
    /** Cleanup authority is the normal controller, never the dead resident guardian. */
    void requireNormalRecoveryLock()throws Exception {
        if(guardian!=0||!authority(true))throw new IOException("Normal recovery root authority unavailable");
        Path lock=path("state/lifecycle.lock");stamp(lock,false);
        Object key=Files.readAttributes(lock,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS).fileKey();
        if(!key.equals(Files.readAttributes(Paths.get("/proc/self/fd/9"),BasicFileAttributes.class).fileKey()))throw new IOException("Normal recovery lifecycle descriptor mismatch");
        int current=Integer.parseInt(new String(read(Paths.get("/proc/self/stat"),8192),StandardCharsets.US_ASCII).split(" ",2)[0]);
        for(int i=0;i<12&&current>1;i++) {
            String command=new String(read(Paths.get("/proc/"+current+"/cmdline"),8192),StandardCharsets.US_ASCII);
            if(command.contains(module+"/control.sh\0rollback\0")||command.contains(module+"/control.sh\0rollback-trial\0")||command.contains(module+"/control.sh\0rollback-health\0")) {
                if(!rootProcessStatus(current)||!key.equals(Files.readAttributes(Paths.get("/proc/"+current+"/fd/9"),BasicFileAttributes.class).fileKey()))throw new IOException("Normal controller lock/authority changed");
                return;
            }
            String stat=new String(read(Paths.get("/proc/"+current+"/stat"),8192),StandardCharsets.US_ASCII);int close=stat.lastIndexOf(") ");if(close<0)break;
            String[] fields=stat.substring(close+2).trim().split("\\s+");if(fields.length<2||!fields[1].matches("[0-9]{1,10}"))break;current=Integer.parseInt(fields[1]);
        }
        throw new IOException("Only the inherited normal rollback controller may reconcile");
    }
    String stockNormal()throws Exception {
        packageHash();return execute(Arrays.asList("/system/bin/sh",module+"/latency-apply-normal.sh"),8000,4096);
    }
    void requireInheritedGuardian(String operation,int pid,String ticks)throws Exception {
        requireInheritedGuardian(operation,pid,ticks,false);
    }
    /** Authenticate the ancestor without keeping its lifecycle lock in Java. */
    void requireResidentGuardian(String operation,int pid,String ticks)throws Exception {
        if(pid!=guardian||!Objects.equals(ticks,guardianStart)||!authority()||!operation.equals(currentOperation()))throw new IOException("Resident guardian authority changed");
        Path lock=path("state/lifecycle.lock");stamp(lock,false);Object key=Files.readAttributes(lock,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS).fileKey();
        if(!key.equals(Files.readAttributes(Paths.get("/proc/"+pid+"/fd/9"),BasicFileAttributes.class).fileKey()))throw new IOException("Guardian does not retain the lifecycle descriptor");
        // ART may reuse number 9 for an unrelated file; only the lock is forbidden.
        try{if(key.equals(Files.readAttributes(Paths.get("/proc/self/fd/9"),BasicFileAttributes.class).fileKey()))throw new IOException("Resident child retained the lifecycle lock");}
        catch(NoSuchFileException closed) { }
        String command=new String(read(Paths.get("/proc/"+pid+"/cmdline"),8192),StandardCharsets.US_ASCII);
        if(!command.contains(module+"/latency-apply.sh\0--worker\0"+operation+"\0"))throw new IOException("Unexpected resident guardian command");
        int current=Integer.parseInt(new String(read(Paths.get("/proc/self/stat"),8192),StandardCharsets.US_ASCII).split(" ",2)[0]);boolean ancestor=false;
        for(int i=0;i<8&&current>1;i++) {
            if(current==pid){ancestor=true;break;}
            String stat=new String(read(Paths.get("/proc/"+current+"/stat"),8192),StandardCharsets.US_ASCII);int close=stat.lastIndexOf(") ");if(close<0)break;
            String[] fields=stat.substring(close+2).trim().split("\\s+");if(fields.length<2||!fields[1].matches("[0-9]{1,10}"))break;current=Integer.parseInt(fields[1]);
        }
        if(!ancestor)throw new IOException("Resident guardian is not this process's ancestor");
        ApplyStore.Record r=new ApplyStore(path("state/latency-application"),0).load(operation);
        if(r==null||!r.boot.equals(boot())||r.workerPid!=0&&(r.workerPid!=pid||!r.workerStart.equals(ticks))||ApplyOperation.terminal(r.phase))throw new IOException("Resident operation ownership changed");
    }
    void requireInheritedGuardian(String operation,int pid,String ticks,boolean cleanup)throws Exception {
        if(pid!=guardian||!Objects.equals(ticks,guardianStart)||!authority(cleanup)||!operation.equals(currentOperation()))throw new IOException("Guardian authority changed");
        Path lock=path("state/lifecycle.lock");stamp(lock,false);
        Object key=Files.readAttributes(lock,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS).fileKey();
        if(!key.equals(Files.readAttributes(Paths.get("/proc/self/fd/9"),BasicFileAttributes.class).fileKey())||
            !key.equals(Files.readAttributes(Paths.get("/proc/"+pid+"/fd/9"),BasicFileAttributes.class).fileKey()))throw new IOException("Inherited lifecycle descriptor mismatch");
        String command=new String(read(Paths.get("/proc/"+pid+"/cmdline"),8192),StandardCharsets.US_ASCII);
        if(!command.contains(module+"/latency-apply.sh\0--worker\0"+operation+"\0"))throw new IOException("Unexpected lifecycle guardian command");
        int current=Integer.parseInt(new String(read(Paths.get("/proc/self/stat"),8192),StandardCharsets.US_ASCII).split(" ",2)[0]);boolean ancestor=false;
        for(int i=0;i<8&&current>1;i++) {
            if(current==pid){ancestor=true;break;}
            String stat=new String(read(Paths.get("/proc/"+current+"/stat"),8192),StandardCharsets.US_ASCII);int close=stat.lastIndexOf(") ");if(close<0)break;
            String[] fields=stat.substring(close+2).trim().split("\\s+");if(fields.length<2||!fields[1].matches("[0-9]{1,10}"))break;current=Integer.parseInt(fields[1]);
        }
        if(!ancestor)throw new IOException("Guardian is not this fixed helper's ancestor");
    }
    public String command(RootFacts.Command command,int oldHal)throws Exception {
        packageHash();List<String> args=new ArrayList<>();int timeout=8000;
        switch(command) {
        case CONFIG:args=Arrays.asList("/system/bin/sh",module+"/config.sh","tuning","get");break;
        case RUNTIME:args=Arrays.asList("/system/bin/sh",module+"/latency-runtime.sh","status");break;
        case RECOVERY:args=Arrays.asList("/system/bin/sh",module+"/latency-runtime.sh","recovery-status");break;
        case PROVIDER:args=Arrays.asList(module+"/latency-provider","--status","--module-dir",module.toString());break;
        case PREFLIGHT:args=Arrays.asList("/system/bin/sh",module+"/latency-apply-read.sh","preflight");break;
        case NORMAL:if(oldHal<=1)throw new IOException("Original HAL is required");args=Arrays.asList("/system/bin/sh",module+"/latency-apply-read.sh","normal",""+hal(),""+oldHal);break;
        case SELECTION:
            args=Arrays.asList("/system/bin/app_process","/system/bin","AndroidSelectionFacts");break;
        default:throw new IOException("Unknown fixed read-only command");
        }
        return execute(args,timeout,65536);
    }
    private String execute(List<String> args,long timeout,int maximum)throws Exception {
        ProcessBuilder builder=new ProcessBuilder(args).redirectErrorStream(true);builder.environment().put("CLASSPATH",module+"/latency-application.dex");
        Process process=builder.start();ByteArrayOutputStream bytes=new ByteArrayOutputStream();final Throwable[] failure={null};
        Thread reader=new Thread(()->{try(InputStream in=process.getInputStream()){byte[] buffer=new byte[1024];int n;while((n=in.read(buffer))!=-1){synchronized(bytes){if(bytes.size()+n>maximum)throw new IOException("Fixed helper output exceeds bound");bytes.write(buffer,0,n);}}}catch(Throwable error){failure[0]=error;process.destroy();}},"Shizuka fixed fact reader");reader.setDaemon(true);reader.start();
        try {
            if(!process.waitFor(timeout,TimeUnit.MILLISECONDS)){process.destroy();if(!process.waitFor(200,TimeUnit.MILLISECONDS))process.destroyForcibly();throw new IOException("Fixed read-only helper timed out");}
            reader.join(500);if(reader.isAlive()||failure[0]!=null||process.exitValue()!=0)throw new IOException("Fixed read-only helper failed");
            return ApplicationInputs.utf8(bytes.toByteArray());
        }finally{process.destroy();reader.interrupt();}
    }
}
