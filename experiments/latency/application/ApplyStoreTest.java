// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Comparator;

public final class ApplyStoreTest {
    static int checks;
    interface Task {void run() throws Exception;}
    static void check(boolean okay){checks++;if(!okay)throw new AssertionError("check "+checks);}
    static void rejects(Task task) throws Exception {
        try{task.run();throw new AssertionError("expected rejection");}catch(IOException|IllegalArgumentException expected){checks++;}
    }
    static String id(int n){return String.format("%08x-41da-4c3b-baf7-15350e1acc7e",n);}
    public static void main(String[] args) throws Exception {
        Path temp=Files.createTempDirectory("apply-store-test-");
        try {
            int uid=((Number)Files.getAttribute(temp,"unix:uid")).intValue();Path directory=temp.resolve("journal");
            ApplyStore store=new ApplyStore(directory,uid);
            ApplyOperation.Request a=new ApplyOperation.Request(id(1),150,0),b=new ApplyOperation.Request(id(2),50,100);
            check(store.claim(a,ApplyOperationTest.BOOT,1000)==ApplyStore.Claim.NEW);
            check(store.claim(a,ApplyOperationTest.BOOT,1001)==ApplyStore.Claim.REUSED);
            check(store.claim(new ApplyOperation.Request(a.id,149,0),ApplyOperationTest.BOOT,1001)==ApplyStore.Claim.CONFLICT);
            check(store.claim(b,ApplyOperationTest.BOOT,1001)==ApplyStore.Claim.BUSY);
            store.bind(a.id,41,"100",1002);store.bind(a.id,41,"100",1002);
            rejects(()->store.bind(a.id,42,"101",1003));
            store.publish(a.id,ApplyOperation.Phase.PREFLIGHT,null,"check",1004);
            store.publish(a.id,ApplyOperation.Phase.RECONFIGURING,ApplyOperationTest.snapshot(true,44100,44100),"owned",1005);
            ApplyStore.Record saved=store.load(a.id);
            check(saved.workerPid==41 && saved.workerStart.equals("100") && saved.snapshot.playing);
            check(saved.snapshot.token.equals(ApplyOperationTest.HASH));
            check(saved.sequence==4 && saved.created==1000 && saved.updated==1005);
            rejects(()->store.publish(a.id,ApplyOperation.Phase.VERIFYING,null,"clock",1004));
            store.publish(a.id,ApplyOperation.Phase.FAILED,null,"unknown teardown",1006);
            check(store.claim(b,ApplyOperationTest.BOOT,1007)==ApplyStore.Claim.BUSY); // Terminal failure does not discard unresolved ownership.
            store.resolveOwned(a.id,true,"owned recovery verified",1008);
            check(store.claim(b,ApplyOperationTest.BOOT,1009)==ApplyStore.Claim.NEW);
            check(store.currentId().equals(b.id));
            store.resolveVerifiedOrphan(b.id,"055c0fe3-d91c-44ab-8700-5220f57d0d66","New boot stock proof",9000);
            check(store.load(b.id).phase==ApplyOperation.Phase.RECOVERED&&store.load(b.id).updated==1009);
            rejects(()->store.resolveVerifiedOrphan(b.id,ApplyOperationTest.BOOT,"Duplicate resolution",2000));
            rejects(()->store.resolveVerifiedOrphan(b.id,"invalid","Invalid boot",2000));
            check(store.claim(a,ApplyOperationTest.BOOT,1010)==ApplyStore.Claim.REUSED);
            rejects(()->store.publish(a.id,ApplyOperation.Phase.APPLIED,null,"old worker",1011));
            rejects(()->store.load("../../bad"));

            Path file=directory.resolve(b.id+".bin");byte[] good=Files.readAllBytes(file);
            byte[] corrupt=good.clone();corrupt[15]^=1;Files.write(file,corrupt);rejects(()->store.load(b.id));Files.write(file,good);
            Files.setPosixFilePermissions(file,PosixFilePermissions.fromString("rw-r--r--"));rejects(()->store.load(b.id));
            Files.setPosixFilePermissions(file,PosixFilePermissions.fromString("rw-------"));
            Path link=temp.resolve("journal-hardlink");Files.createLink(link,file);rejects(()->store.load(b.id));Files.delete(link);
            Files.delete(file);Files.createSymbolicLink(file,temp.resolve("absent"));rejects(()->store.load(b.id));Files.delete(file);Files.write(file,good);
            Files.setPosixFilePermissions(file,PosixFilePermissions.fromString("rw-------"));
            Files.write(file,new byte[ApplyStore.MAX_RECORD+1]);rejects(()->store.load(b.id));Files.write(file,good);
            rejects(()->new ApplyStore(directory,uid+1));
            Path moved=temp.resolve("old-journal");Files.move(directory,moved);
            Files.createDirectory(directory,PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
            rejects(()->store.load(b.id));
            ApplyStore clean=new ApplyStore(temp.resolve("bounded"),uid);
            for(int i=0;i<ApplyStore.MAX_OPERATIONS;i++) {
                ApplyOperation.Request request=new ApplyOperation.Request(id(1000+i),1,0);
                check(clean.claim(request,ApplyOperationTest.BOOT,2000+i*2)==ApplyStore.Claim.NEW);
                clean.publish(request.id,ApplyOperation.Phase.REJECTED,null,"no mutation",2001+i*2);
            }
            check(clean.claim(new ApplyOperation.Request(id(9999),1,0),ApplyOperationTest.BOOT,9999)==ApplyStore.Claim.FULL);
            System.out.println("Private apply journal: "+checks+" ownership/idempotency/atomic-format checks passed.");
        } finally {
            try(java.util.stream.Stream<Path> paths=Files.walk(temp)){paths.sorted(Comparator.reverseOrder()).forEach(p->{try{Files.deleteIfExists(p);}catch(IOException ignored){}});}
        }
    }
}
