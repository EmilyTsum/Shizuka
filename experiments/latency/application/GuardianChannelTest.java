// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.charset.StandardCharsets;

public final class GuardianChannelTest {
    static int checks;
    static void check(boolean value){checks++;if(!value)throw new AssertionError("channel check "+checks);}
    static void reply(Path dir,String text)throws Exception {
        Path temporary=Files.createTempFile(dir,".reply-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        Files.write(temporary,text.getBytes(StandardCharsets.US_ASCII));Files.delete(dir.resolve("request"));
        Files.move(temporary,dir.resolve("response"),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("shizuka-guardian-test-");int owner=((Number)Files.getAttribute(root,"unix:uid")).intValue();
        try {
            Path dir=root.resolve("channel");GuardianChannel channel=new GuardianChannel(dir,owner);
            ApplyOperation.Request r=new ApplyOperation.Request(ApplyOperationTest.ID,150,100);ApplyOperation.Snapshot s=ApplyOperationTest.snapshot(false,44100,44100);
            final Throwable[] failure={null};Thread guardian=new Thread(()->{try {
                for(int sequence=1;sequence<=2;sequence++) {
                    while(!Files.exists(dir.resolve("request")))Thread.sleep(5);
                    String[] fields=new String(Files.readAllBytes(dir.resolve("request")),StandardCharsets.US_ASCII).trim().split(" ");
                    check(fields.length==10&&fields[0].equals("SHZAPPLY1")&&fields[1].equals(""+sequence)&&fields[2].equals(""+sequence));
                    check(fields[3].equals(r.id)&&fields[4].equals("150")&&fields[5].equals("100")&&fields[7].equals(s.token)&&fields[8].equals(s.configIdentity));
                    reply(dir,"SHZAPPLY1 "+sequence+" "+sequence+" "+r.id+" "+(sequence==1?0:30)+"\n");
                }
            }catch(Throwable error){failure[0]=error;}});guardian.start();
            channel.invoke(GuardianChannel.Action.ARM,r,s,false,System.nanoTime()/1000000+2000);check(true);
            try {channel.invoke(GuardianChannel.Action.RECREATE,r,s,false,System.nanoTime()/1000000+2000);throw new AssertionError("unsafe success");}
            catch(java.io.IOException expected){check(expected.getMessage().contains("recovery"));}
            guardian.join(3000);if(failure[0]!=null)throw new AssertionError(failure[0]);check(!guardian.isAlive());
            Files.delete(dir.resolve("response"));
            try {channel.invoke(GuardianChannel.Action.FINISH,r,s,false,System.nanoTime()/1000000+80);throw new AssertionError("unsafe timeout success");}
            catch(java.io.IOException expected){check(Files.exists(dir.resolve("request")));}
            Files.delete(dir.resolve("request"));Files.createSymbolicLink(dir.resolve("response"),root.resolve("outside"));
            try {channel.invoke(GuardianChannel.Action.FINISH,r,s,false,System.nanoTime()/1000000+1000);throw new AssertionError("unsafe symlink success");}
            catch(java.io.IOException expected){check(true);}
            System.out.println("Guardian channel: "+checks+" typed/bound/recovery/timeout/private-file checks passed.");
        }finally{try(java.util.stream.Stream<Path> files=Files.walk(root)){for(Path p:(Iterable<Path>)files.sorted(java.util.Comparator.reverseOrder())::iterator)Files.deleteIfExists(p);}}
    }
}
