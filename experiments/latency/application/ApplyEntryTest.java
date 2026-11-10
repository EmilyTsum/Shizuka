// SPDX-License-Identifier: GPL-2.0-only
import java.nio.file.*;
import java.util.*;

public final class ApplyEntryTest {
    static int checks;static void check(boolean value){checks++;if(!value)throw new AssertionError("entry check "+checks);}
    static final class Result implements ApplyEntry.Completion {int calls;ApplyCommand.Output out;Exception error;public void finished(ApplyCommand.Output o,Exception e){calls++;out=o;error=e;}}
    static final class Factory implements ApplyEntry.Factory {
        boolean enabled=true;int gates,reads,workers;String selected;ApplyEntry.Completion pending;ApplyOperationTest.Fake adapter;
        Factory(ApplyOperationTest.Fake adapter){this.adapter=adapter;}
        public boolean enabled(){gates++;return enabled;}
        public ApplyOperation.Adapter readOnly(String id){reads++;selected=id;return adapter;}
        public void worker(ApplyInvocation i,ApplyCommand.Environment e,ApplyEntry.Completion c){workers++;pending=c;}
        public void loop(){throw new AssertionError("The dispatcher cannot enter a platform loop itself");}
    }
    static int sequence;
    static ApplyCommandTest.Env env(Path parent,ApplyOperationTest.Fake adapter)throws Exception {return new ApplyCommandTest.Env(Files.createDirectory(parent.resolve("case"+(++sequence))),adapter);}
    static String[] worker(){return new String[]{"worker",ApplyOperationTest.ID,"123","456"};}
    static String[] prepare(){return new String[]{"prepare",ApplyOperationTest.ID,"150","0"};}
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("apply-entry-test-");
        try {
            ApplyOperationTest.Fake a=new ApplyOperationTest.Fake();ApplyCommandTest.Env e=env(root,a);Factory f=new Factory(a);f.enabled=false;Result r=new Result();
            check(!ApplyEntry.start(prepare(),e,f,r)&&r.calls==1&&r.error==null&&r.out.json.get("phase").equals("rejected"));
            check(f.reads==0&&f.workers==0&&e.opened==0&&e.journal.currentId()==null);
            e.journal.claim(new ApplyOperation.Request(ApplyOperationTest.ID,150,0),e.boot(),e.now());long before=e.journal.load(ApplyOperationTest.ID).sequence;
            r=new Result();check(!ApplyEntry.start(worker(),e,f,r)&&r.calls==1&&r.error!=null);
            check(e.identities==0&&f.workers==0&&e.journal.load(ApplyOperationTest.ID).sequence==before&&e.journal.load(ApplyOperationTest.ID).workerPid==0);
            for(String[] invalid:new String[][]{new String[]{},new String[]{"status","../bad"},new String[]{"prepare",ApplyOperationTest.ID,"0150","0"},new String[]{"prepare",ApplyOperationTest.ID,"1","101"},new String[]{"worker",ApplyOperationTest.ID,"1","456"},new String[]{"worker",ApplyOperationTest.ID,"2147483648","456"},new String[]{"worker",ApplyOperationTest.ID,"123","9223372036854775808"}}) {
                Factory bad=new Factory(a);Result rejected=new Result();check(!ApplyEntry.start(invalid,e,bad,rejected)&&rejected.calls==1&&rejected.error!=null&&bad.gates==0&&bad.reads==0&&bad.workers==0);
            }
            a=new ApplyOperationTest.Fake();e=env(root,a);f=new Factory(a);r=new Result();
            check(!ApplyEntry.start(prepare(),e,f,r)&&r.error==null&&r.out.code==10&&f.reads==1&&f.workers==0&&f.selected.equals(ApplyOperationTest.ID));
            r=new Result();check(!ApplyEntry.start(new String[]{"status"},e,f,r)&&r.error==null&&f.selected.equals(ApplyOperationTest.ID)&&f.workers==0);
            String unknown="fd22fba8-41da-4c3b-baf7-15350e1acc7e";r=new Result();check(!ApplyEntry.start(new String[]{"status",unknown},e,f,r)&&r.error==null&&f.selected.equals(unknown)&&Boolean.FALSE.equals(r.out.json.get("operation_known")));
            r=new Result();check(ApplyEntry.start(worker(),e,f,r)&&f.workers==1&&r.calls==0&&e.journal.load(ApplyOperationTest.ID).workerPid==0);
            ApplyCommand.Output finalOut=new ApplyCommand.Output(0,ControlJson.object("phase","unconfirmed"));f.pending.finished(finalOut,null);f.pending.finished(null,new IllegalStateException("late"));check(r.calls==1&&r.out==finalOut&&r.error==null);
            a.time+=20001;r=new Result();int old=f.workers;check(!ApplyEntry.start(worker(),e,f,r)&&r.error!=null&&f.workers==old);
            a=new ApplyOperationTest.Fake();e=env(root,a);f=new Factory(a);e.root=false;r=new Result();check(!ApplyEntry.start(prepare(),e,f,r)&&r.error!=null&&f.gates==0&&f.reads==0);
            a=new ApplyOperationTest.Fake();e=env(root,a);f=new Factory(a);e.journal.claim(new ApplyOperation.Request(ApplyOperationTest.ID,150,0),e.boot(),e.now());e.journal.bind(ApplyOperationTest.ID,123,"456",e.now());r=new Result();
            check(!ApplyEntry.start(worker(),e,f,r)&&r.error!=null&&f.workers==0);
            a=new ApplyOperationTest.Fake();e=env(root,a);f=new Factory(a);e.journal.claim(new ApplyOperation.Request(ApplyOperationTest.ID,150,0),"055c0fe3-d91c-44ab-8700-5220f57d0d66",e.now());r=new Result();
            check(!ApplyEntry.start(worker(),e,f,r)&&r.error!=null&&f.workers==0);
            System.setProperty("shizuka.application.qualified","true");System.setProperty("QUALIFIED","1");
            check(!ApplicationQualification.residentFactory()&&LatencyApplyMain.factory(root)==ApplyEntry.UNQUALIFIED);
            System.out.println("Application entry: "+checks+" dispatch/queue/no-service/unqualified/once-only checks passed.");
        }finally{try(java.util.stream.Stream<Path> paths=Files.walk(root)){for(Path p:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.deleteIfExists(p);}}
    }
}
