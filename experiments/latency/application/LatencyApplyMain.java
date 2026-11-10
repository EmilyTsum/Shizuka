// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.math.BigInteger;
import java.util.Arrays;

/** Fixed application dispatcher; the resident factory remains source-disabled. */
public final class LatencyApplyMain {
    private static final class RootEnvironment implements ApplyCommand.Environment {
        final Path module,state;final String boot;
        RootEnvironment(String path)throws Exception {
            module=Paths.get(path);
            if(!module.isAbsolute() || !module.normalize().equals(module) || !module.toRealPath().equals(module))
                throw new IOException("Module path must be absolute and canonical");
            trustedDirectory(module,false);state=module.resolve("state");trustedDirectory(state,true);
            boot=small(Paths.get("/proc/sys/kernel/random/boot_id"),64).trim();
            if(!boot.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}"))throw new IOException("Invalid current boot");
        }
        public ApplyStore store()throws Exception{return new ApplyStore(state.resolve("latency-application"),0);}
        public String boot(){return boot;}
        public long now(){return System.nanoTime()/1000000L;}
        public void authority()throws Exception {
            String status=small(Paths.get("/proc/self/status"),16384);
            String uid=line(status,"Uid:"),caps=line(status,"CapEff:");
            if(!uid.matches("0\\s+0\\s+0\\s+0") || !caps.matches("[a-fA-F0-9]{1,16}") || !new BigInteger(caps,16).testBit(21))
                throw new IOException("Current root CAP_SYS_ADMIN authority is required");
            trustedDirectory(module,false);trustedDirectory(state,true);
            if(Files.exists(module.resolve("disable"),LinkOption.NOFOLLOW_LINKS)||Files.exists(module.resolve("remove"),LinkOption.NOFOLLOW_LINKS))
                throw new IOException("Module is disabled or removing");
            if(!boot.equals(small(Paths.get("/proc/sys/kernel/random/boot_id"),64).trim()))throw new IOException("Boot changed");
        }
        public void workerIdentity(int pid,String start)throws Exception {
            String stat=small(Paths.get("/proc/"+pid+"/stat"),8192);int split=stat.lastIndexOf(") ");
            String[] fields=split<0?new String[0]:stat.substring(split+2).trim().split("\\s+");
            if(fields.length<20 || fields[0].equals("Z") || fields[0].equals("X") || !start.equals(fields[19]))
                throw new IOException("Lifecycle guardian identity changed");
            String status=small(Paths.get("/proc/"+pid+"/status"),16384);
            if(!line(status,"Uid:").matches("0\\s+0\\s+0\\s+0"))throw new IOException("Guardian is not root");
        }
    }
    private static String line(String body,String key)throws IOException {
        String found=null;for(String line:body.split("\n"))if(line.startsWith(key)){if(found!=null)throw new IOException("Duplicate process field");found=line.substring(key.length()).trim();}
        if(found==null)throw new IOException("Missing process field");return found;
    }
    private static String small(Path path,int maximum)throws IOException {
        try(java.io.InputStream input=Files.newInputStream(path,LinkOption.NOFOLLOW_LINKS);java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream()) {
            byte[] buffer=new byte[1024];int count;while((count=input.read(buffer))!=-1){if(out.size()+count>maximum)throw new IOException("Input exceeds bound");out.write(buffer,0,count);}
            return new String(out.toByteArray(),StandardCharsets.US_ASCII);
        }
    }
    private static void trustedDirectory(Path directory,boolean privateMode)throws IOException {
        if(!Files.isDirectory(directory,LinkOption.NOFOLLOW_LINKS)||((Number)Files.getAttribute(directory,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=0)
            throw new IOException("Unsafe root directory");
        java.util.Set<java.nio.file.attribute.PosixFilePermission> permissions=Files.getPosixFilePermissions(directory,LinkOption.NOFOLLOW_LINKS);
        if(privateMode?!permissions.equals(PosixFilePermissions.fromString("rwx------")):
                permissions.contains(java.nio.file.attribute.PosixFilePermission.GROUP_WRITE)||permissions.contains(java.nio.file.attribute.PosixFilePermission.OTHERS_WRITE))
            throw new IOException("Unsafe root directory permissions");
    }
    static ApplyEntry.Factory factory(Path module)throws Exception {
        if(!ApplicationQualification.residentFactory())return ApplyEntry.UNQUALIFIED;
        // Fixed source-bound class only. No caller-supplied class name, executor
        // path or alternate classpath is accepted by the verified shell entry.
        return (ApplyEntry.Factory)Class.forName("AndroidApplicationFactory").getDeclaredConstructor(Path.class).newInstance(module);
    }
    private static void complete(ApplyCommand.Output out,Exception failure) {
        if(failure!=null){System.err.println("Shizuka delay application: "+failure.getClass().getSimpleName()+": "+failure.getMessage());System.exit(1);}
        System.out.println(ControlJson.encode(out.json));System.exit(out.code);
    }
    public static void main(String[] args) {
        try {
            if(args.length<3 || !args[0].equals("--module-dir"))throw new IllegalArgumentException("--module-dir ABSOLUTE_MODULE_PATH COMMAND");
            RootEnvironment env=new RootEnvironment(args[1]);
            ApplyEntry.Factory factory=factory(env.module);
            if(ApplyEntry.start(Arrays.copyOfRange(args,2,args.length),env,factory,LatencyApplyMain::complete))factory.loop();
            throw new IllegalStateException("The application entry returned without a final result");
        } catch(Exception failure) {
            System.err.println("Shizuka delay application: "+failure.getClass().getSimpleName()+": "+failure.getMessage());System.exit(1);
        }
    }
}
