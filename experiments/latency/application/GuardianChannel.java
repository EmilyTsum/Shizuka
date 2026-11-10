// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.*;

/** One resident worker, one private guardian. Protocol contains typed data, never shell text. */
final class GuardianChannel {
    enum Action { ARM(1),RECREATE(2),REMOVE(3),FINISH(4),RECOVER(5);final int code;Action(int code){this.code=code;} }
    private final Path directory;
    private final Object key;
    private final int owner;
    private int sequence;
    GuardianChannel(Path directory,int owner)throws IOException {
        this.directory=directory;this.owner=owner;
        Files.createDirectories(directory,PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        key=attributes(directory,true).fileKey();if(key==null)throw new IOException("Channel identity unavailable");
    }
    private BasicFileAttributes attributes(Path path,boolean directory)throws IOException {
        BasicFileAttributes a=Files.readAttributes(path,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        if(!(directory?a.isDirectory():a.isRegularFile())||((Number)Files.getAttribute(path,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=owner||
                !Files.getPosixFilePermissions(path,LinkOption.NOFOLLOW_LINKS).equals(PosixFilePermissions.fromString(directory?"rwx------":"rw-------"))||
                !directory&&((Number)Files.getAttribute(path,"unix:nlink",LinkOption.NOFOLLOW_LINKS)).intValue()!=1)
            throw new IOException("Unsafe private guardian channel");return a;
    }
    private void pinned()throws IOException {if(!key.equals(attributes(directory,true).fileKey()))throw new IOException("Guardian channel directory changed");}
    synchronized void invoke(Action action,ApplyOperation.Request request,ApplyOperation.Snapshot snapshot,boolean flag,long deadline)throws Exception {
        if(action==null||snapshot==null||!snapshot.token.matches("[a-f0-9]{64}")||!snapshot.configIdentity.matches("[a-f0-9]{64}")||
                deadline<=now()||deadline>now()+210000||sequence>=10000)throw new IllegalArgumentException("Invalid guardian request");
        pinned();int next=++sequence;
        String line="SHZAPPLY1 "+next+" "+action.code+" "+request.id+" "+request.hqMs+" "+request.llMs+" "+deadline+" "+snapshot.token+" "+snapshot.configIdentity+" "+(flag?1:0)+"\n";
        Path temporary=Files.createTempFile(directory,".request-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            Files.write(temporary,line.getBytes(StandardCharsets.US_ASCII),StandardOpenOption.TRUNCATE_EXISTING,LinkOption.NOFOLLOW_LINKS);
            pinned();if(Files.exists(directory.resolve("request"),LinkOption.NOFOLLOW_LINKS))throw new IOException("Previous guardian request is unresolved");
            Files.move(temporary,directory.resolve("request"),StandardCopyOption.ATOMIC_MOVE);
        }finally{Files.deleteIfExists(temporary);}
        while(now()<deadline) {
            pinned();Path response=directory.resolve("response");
            if(Files.exists(response,LinkOption.NOFOLLOW_LINKS)) {
                BasicFileAttributes before=attributes(response,false);if(before.size()<1||before.size()>256)throw new IOException("Invalid guardian response length");
                byte[] bytes;
                try(java.io.InputStream input=Files.newInputStream(response,LinkOption.NOFOLLOW_LINKS);java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream()) {
                    byte[] buffer=new byte[257];int count;
                    while((count=input.read(buffer))!=-1){if(out.size()+count>256)throw new IOException("Guardian response grew beyond bound");out.write(buffer,0,count);}
                    bytes=out.toByteArray();
                }
                BasicFileAttributes after=attributes(response,false);pinned();
                if(!before.fileKey().equals(after.fileKey())||before.size()!=after.size()||!before.lastModifiedTime().equals(after.lastModifiedTime()))continue;
                String text=new String(bytes,StandardCharsets.US_ASCII);
                if(!text.matches("SHZAPPLY1 [1-9][0-9]{0,4} [1-5] [a-f0-9-]{36} (0|10|20|30)\n"))throw new IOException("Malformed guardian response");
                String[] fields=text.trim().split(" ");int observed=Integer.parseInt(fields[1]);
                if(observed>next)throw new IOException("Future guardian response");
                if(observed==next) {
                    if(!fields[2].equals(""+action.code)||!fields[3].equals(request.id))throw new IOException("Guardian response binding mismatch");
                    int status=Integer.parseInt(fields[4]);if(status!=0)throw new IOException(status==30?"Verified owned recovery is required":status==10?"Fixed lifecycle executor is unavailable":"Fixed lifecycle action failed; ownership remains unresolved");
                    return;
                }
            }Thread.sleep(25);
        }throw new IOException("Guardian response deadline expired; ownership remains unresolved");
    }
    private static long now(){return System.nanoTime()/1000000L;}
}
