// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Map;

public final class ApplyCommandTest {
    static int checks;
    static void check(boolean value){checks++;if(!value)throw new AssertionError("command check "+checks);}
    interface Throwing{void run()throws Exception;}
    static void rejects(Throwing action)throws Exception{try{action.run();throw new AssertionError("expected rejection");}catch(IllegalArgumentException|IllegalStateException expected){checks++;}}
    static final class Env implements ApplyCommand.Environment {
        ApplyStore journal;ApplyOperationTest.Fake adapter;int opened,identities;boolean root=true;
        Env(Path directory,ApplyOperationTest.Fake adapter)throws Exception {
            this.adapter=adapter;int owner=((Number)Files.getAttribute(directory,"unix:uid")).intValue();journal=new ApplyStore(directory.resolve("journal"),owner);
        }
        public ApplyStore store(){opened++;return journal;}
        public String boot(){return ApplyOperationTest.BOOT;}
        public long now(){return adapter.time;}
        public void authority(){if(!root)throw new IllegalStateException("root lost");}
        public void workerIdentity(int pid,String start){identities++;if(pid!=123 || !start.equals("456"))throw new IllegalStateException("guardian changed");}
    }
    static String[] prepare(String id,int hq,int ll){return new String[]{"prepare",id,""+hq,""+ll};}
    static String[] worker(String id){return new String[]{"worker",id,"123","456"};}
    public static void main(String[] args)throws Exception {
        Path temporary=Files.createTempDirectory("shizuka-apply-command-",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        try {
            ApplyOperationTest.Fake a=new ApplyOperationTest.Fake();a.qualified=false;Env env=new Env(temporary,a);
            ApplyCommand.Output out=ApplyCommand.run(prepare(ApplyOperationTest.ID,150,100),env,a);
            check(out.code==0 && Boolean.FALSE.equals(out.json.get("accepted")) && out.json.get("phase").equals("rejected"));
            check(env.opened==0 && env.journal.currentId()==null && a.arms==0 && a.recreates==0);
            out=ApplyCommand.run(new String[]{"status",ApplyOperationTest.ID},env,a);
            check(Boolean.FALSE.equals(out.json.get("operation_known")) && out.json.get("operation_id").equals(ApplyOperationTest.ID) && out.json.get("current_operation_id")==null && Boolean.FALSE.equals(out.json.get("retry_safe")));
            out=ApplyCommand.run(new String[]{"status"},env,a);
            check(Boolean.FALSE.equals(out.json.get("operation_known")) && out.json.get("operation_id")==null && out.json.get("phase").equals("saved_pending") && Boolean.FALSE.equals(out.json.get("retry_safe")) && out.json.get("current_boot_id").equals(ApplyOperationTest.BOOT));
            env.opened=0;
            a.qualified=true;
            a.ownedRecovery=true;
            out=ApplyCommand.run(prepare(ApplyOperationTest.ID,150,100),env,a);
            check(out.code==0 && out.json.get("phase").equals("unconfirmed") && Boolean.FALSE.equals(out.json.get("retry_safe")) && env.opened==0);
            a.ownedRecovery=false;
            a.snapshot=ApplyOperationTest.snapshot(true,96000,96000);
            out=ApplyCommand.run(prepare(ApplyOperationTest.ID,150,100),env,a);
            check(out.code==0 && Boolean.FALSE.equals(out.json.get("accepted")) && env.opened==0 && a.recreates==0);
            a.snapshot=ApplyOperationTest.snapshot(true,44100,44100);
            out=ApplyCommand.run(prepare(ApplyOperationTest.ID,150,100),env,a);
            check(out.code==ApplyCommand.QUEUED && out.json.get("phase").equals("queued") && Boolean.TRUE.equals(out.json.get("accepted")));
            check(ApplyCommand.run(prepare(ApplyOperationTest.ID,150,100),env,a).code==0);
            rejects(()->ApplyCommand.run(prepare(ApplyOperationTest.ID,149,100),env,a));
            String other="fd22fba8-41da-4c3b-baf7-15350e1acc7e";
            out=ApplyCommand.run(prepare(other,150,100),env,a);
            check(Boolean.FALSE.equals(out.json.get("accepted")) && env.journal.load(other)==null);
            a.proof=ApplyOperationTest.proof(new ApplyOperation.Request(ApplyOperationTest.ID,150,100),a.time);
            out=ApplyCommand.run(worker(ApplyOperationTest.ID),env,a);
            check(env.identities==1 && a.recreates==1 && Boolean.TRUE.equals(out.json.get("applied")));
            check(env.journal.load(ApplyOperationTest.ID).workerPid==123 && env.journal.load(ApplyOperationTest.ID).phase==ApplyOperation.Phase.APPLIED);
            rejects(()->ApplyCommand.run(worker(ApplyOperationTest.ID),env,a));
            check(a.recreates==1);
            a.time+=2000;out=ApplyCommand.run(new String[]{"status",ApplyOperationTest.ID},env,a);
            check(out.json.get("phase").equals("unconfirmed") && out.json.get("last_outcome").equals("applied"));
            check(Boolean.TRUE.equals(out.json.get("retry_safe")) && Boolean.FALSE.equals(out.json.get("applied")));
            a.current=false;out=ApplyCommand.run(new String[]{"status",ApplyOperationTest.ID},env,a);
            check(out.json.get("phase").equals("superseded"));
            a.current=true;
            for(String[] bad:new String[][]{new String[]{},new String[]{"unknown"},new String[]{"status","../x"},new String[]{"prepare",other,"015","0"},new String[]{"prepare",other,"151","0"},new String[]{"worker",other,"-1","456"}})
                rejects(()->ApplyCommand.run(bad,env,a));
            out=ApplyCommand.run(prepare(other,150,0),env,a);check(out.code==ApplyCommand.QUEUED);
            a.time+=20001;rejects(()->ApplyCommand.run(worker(other),env,a));check(a.recreates==1);
            env.root=false;rejects(()->ApplyCommand.run(new String[]{"status"},env,a));
            System.out.println("Apply command: "+checks+" qualification/idempotency/guardian/current-proof checks passed.");
        } finally {try(java.util.stream.Stream<Path> files=Files.walk(temporary)){for(Path path:(Iterable<Path>)files.sorted(java.util.Comparator.reverseOrder())::iterator)Files.deleteIfExists(path);}}
    }
}
