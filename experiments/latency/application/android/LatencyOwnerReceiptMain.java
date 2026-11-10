// SPDX-License-Identifier: GPL-2.0-only
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;

/** Resident-owner typed writer/finalizer. Its production source gate remains false. */
public final class LatencyOwnerReceiptMain {
    private static final boolean QUALIFIED=false;
    public static void main(String[] args) {
        if(!QUALIFIED){System.out.println("{\"schema\":1,\"scope\":\"application_owner_receipt\",\"qualified\":false,\"written\":false}");System.exit(10);}
        try {
            if(args.length!=12||!args[0].equals("--module-dir"))throw new IllegalArgumentException("Fixed module + ten owned action arguments are required");
            int action=(int)ApplicationInputs.decimal(args[2],5);String id=args[3];int hq=(int)ApplicationInputs.decimal(args[4],150),ll=(int)ApplicationInputs.decimal(args[5],100);long deadline=ApplicationInputs.decimal(args[6],Long.MAX_VALUE);
            String token=args[7],config=args[8];boolean flag;if(args[9].equals("0"))flag=false;else if(args[9].equals("1"))flag=true;else throw new IllegalArgumentException("Invalid fixed flag");
            int pid=(int)ApplicationInputs.decimal(args[10],Integer.MAX_VALUE);String ticks=args[11];ApplicationInputs.decimal(ticks,Long.MAX_VALUE);
            if(action==0||!id.matches(ApplicationInputs.UUID)||!token.matches(ApplicationInputs.HEX)||!config.matches(ApplicationInputs.HEX))throw new IllegalArgumentException("Invalid owned fields");
            Path module=Paths.get(args[1]);InstalledRootSource source=new InstalledRootSource(module,pid,ticks);source.requireInheritedGuardian(id,pid,ticks);
            Path directory=module.resolve("state/latency-application"),target=directory.resolve("owned."+id);
            ApplyStore store=new ApplyStore(directory,0);ApplyStore.Record journal=store.load(id);
            if(journal==null||journal.workerPid!=pid||!journal.workerStart.equals(ticks)||journal.request.hqMs!=hq||journal.request.llMs!=ll)throw new IllegalArgumentException("Owned journal identity mismatch");
            ApplicationCapture capture=ApplicationCapture.decode(source.file("state/latency-application/channel."+id+"/capture.bin",8192,true));
            if(!capture.request.id.equals(id)||!capture.snapshot.token.equals(token)||!capture.snapshot.configIdentity.equals(config))throw new IllegalArgumentException("Immutable capture binding mismatch");
            ApplicationOwnerRecord old=source.exists("state/latency-application/owned."+id)?ApplicationOwnerRecord.decode(source.file("state/latency-application/owned."+id,1024,true)):null;
            ApplicationOwnerRecord next=ApplicationOwnerWriter.transition(action,flag,source,source::requireInheritedGuardian,journal,capture,old,deadline);
            source.requireInheritedGuardian(id,pid,ticks);
            // Keep the reviewed original cookie/payload lineage available to a
            // later normal owner after an interrupted operation or app update.
            if(action==1)PrivateApplicationFiles.publishOnce(directory.resolve("channel."+id+"/runtime.identity"),source.file("latency-runtime.identity",230,false));
            Path temporary=Files.createTempFile(directory,".owned-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
            try {
                try(FileChannel out=FileChannel.open(temporary,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)){ByteBuffer bytes=ByteBuffer.wrap(next.bytes());while(bytes.hasRemaining())out.write(bytes);out.force(true);}
                Files.move(temporary,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                try(FileChannel parent=FileChannel.open(directory,StandardOpenOption.READ)){parent.force(true);}
            }finally{Files.deleteIfExists(temporary);}
            System.out.println(ControlJson.encode(ControlJson.object("schema",1,"scope","application_owner_receipt","qualified",true,"written",true,"phase",next.phase,"operation_id",id)));
        }catch(Exception failure){System.err.println("Owned receipt unavailable: "+failure.getClass().getSimpleName());System.exit(20);}
    }
}
