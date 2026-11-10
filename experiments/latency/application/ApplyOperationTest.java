// SPDX-License-Identifier: GPL-2.0-only
import java.util.ArrayList;
import java.util.List;

public final class ApplyOperationTest {
    static int checks;
    static void check(boolean okay) { checks++; if(!okay) throw new AssertionError("check "+checks); }
    static final String ID="ed22fba8-41da-4c3b-baf7-15350e1acc7e", BOOT="955c0fe3-d91c-44ab-8700-5220f57d0d66";
    static final String COOKIE="0123456789abcdef0123456789abcdef", HASH=COOKIE+COOKIE;
    static ApplyOperation.Epoch epoch(int pid,String start,long graph) { return new ApplyOperation.Epoch(BOOT,COOKIE,pid,start,graph); }
    static ApplyOperation.Snapshot snapshot(boolean playing,int selectedRate,int actualRate) {
        return new ApplyOperation.Snapshot(epoch(41,"100",3),HASH,HASH,playing,true,true,selectedRate,actualRate,1);
    }
    static ApplyOperation.Proof proof(ApplyOperation.Request request,long now) {
        ApplyOperation.Proof p=new ApplyOperation.Proof();p.epoch=epoch(42,"200",1);
        p.operationId=request.id;p.snapshotToken=HASH;p.configIdentity=HASH;
        p.authenticated=p.leaseValid=p.init=p.open=p.start=p.worker=p.registration=true;
        p.observedAt=now;p.leaseUntil=now+1900;p.providerRevision=1;p.rd=3;p.wr=2;p.confirmed=2;
        p.phase=5;p.profile=1;p.sampleRate=44100;p.mappings=2;p.targetSg=0xb00000abL;p.container=0xe00000e6L;
        p.graphSourcePort=0x2012; // The real small registered source port is valid.
        p.deltaUs=request.hqMs*1000L;p.beforeUs=167000;p.afterUs=p.beforeUs+p.deltaUs;return p;
    }
    static final class Fake implements ApplyOperation.Adapter,ApplyOperation.Journal {
        long time=1000;boolean qualified=true,authority=true,current=true,recoveryOkay=true,ownedRecovery;
        boolean throwArm,throwRecreate,changedAfterRecreate,configAfterProof,loseAuthorityAfterRecreate,expireDuringRecreate;
        boolean resume,mayRestore;int arms,recreates,disables,restores,recoveries,finishes,observations;
        ApplyOperation.Fence fence=ApplyOperation.Fence.SAME;
        ApplyOperation.Snapshot snapshot=snapshot(true,44100,44100);
        ApplyOperation.Proof proof;
        ApplyOperation.DisabledReceipt disabledReceipt;
        List<ApplyOperation.Phase> phases=new ArrayList<>();
        public long now(){return time;}
        public boolean qualified(){return qualified;}
        public boolean disableQualified(){return qualified;}
        public boolean currentContextSupported(){return snapshot!=null && snapshot.supported();}
        public boolean ownedRecoveryRequired(){return ownedRecovery;}
        public boolean authority(){return authority;}
        public boolean requestCurrent(ApplyOperation.Request r,ApplyOperation.Snapshot s){return current;}
        public ApplyOperation.Snapshot capture(ApplyOperation.Request r){return snapshot;}
        public ApplyOperation.Fence fence(ApplyOperation.Snapshot s){return fence;}
        public void armRecovery(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){arms++;if(throwArm)throw new IllegalStateException("partial recovery arm failed");}
        public void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){
            recreates++;if(changedAfterRecreate)fence=ApplyOperation.Fence.USER_CHANGED;
            if(loseAuthorityAfterRecreate)authority=false;if(expireDuringRecreate)time=d;
            if(throwRecreate)throw new IllegalStateException("partial owned recreate failed");
        }
        public void restore(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean resume,long d){restores++;this.resume=resume;}
        public void disable(ApplyOperation.Request r,ApplyOperation.Snapshot s,long d){disables++;}
        public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s){observations++;if(configAfterProof)current=false;return proof;}
        public ApplyOperation.DisabledReceipt observeDisabled(ApplyOperation.Request r,ApplyOperation.Snapshot s){return disabledReceipt;}
        public void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean waiting,long d){finishes++;}
        public boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean allow,long d){recoveries++;mayRestore=allow;return recoveryOkay;}
        public void pause(long millis){time+=millis;}
        public void publish(ApplyOperation.Request r,ApplyOperation.Phase p,ApplyOperation.Snapshot s,String reason){phases.add(p);}
    }
    public static void main(String[] args) throws Exception {
        ApplyOperation.Request r=new ApplyOperation.Request(ID,150,100);
        Fake a=new Fake();a.proof=proof(r,a.time);
        ApplyOperation.Result result=ApplyOperation.run(r,a,a);
        check(result.applied && result.phase==ApplyOperation.Phase.APPLIED);
        check(a.arms==1 && a.recreates==1 && a.restores==1 && a.resume && a.recoveries==0 && a.finishes==1);
        check(a.phases.get(0)==ApplyOperation.Phase.PREFLIGHT && a.phases.get(1)==ApplyOperation.Phase.RECONFIGURING);
        check(a.phases.contains(ApplyOperation.Phase.RESTORING) && a.phases.contains(ApplyOperation.Phase.VERIFYING));

        Fake idle=new Fake();idle.snapshot=snapshot(false,44100,44100);
        result=ApplyOperation.run(r,idle,idle);
        check(result.phase==ApplyOperation.Phase.WAITING_FOR_PLAYBACK && !result.applied);
        check(idle.restores==1 && !idle.resume && idle.observations==0 && idle.finishes==1);
        Fake firstIdle=new Fake();firstIdle.snapshot=new ApplyOperation.Snapshot(epoch(41,"100",0),HASH,HASH,false,true,false,44100,0,0,true,true);
        result=ApplyOperation.run(r,firstIdle,firstIdle);
        check(firstIdle.snapshot.supported()&&!firstIdle.snapshot.reviewedHq&&firstIdle.snapshot.profile==0&&firstIdle.snapshot.observedRate==0);
        check(result.phase==ApplyOperation.Phase.WAITING_FOR_PLAYBACK&&!result.applied&&!firstIdle.resume&&firstIdle.observations==0);
        Fake firstPlaying=new Fake();firstPlaying.snapshot=new ApplyOperation.Snapshot(epoch(41,"100",0),HASH,HASH,true,true,false,44100,0,0,true,true);firstPlaying.proof=proof(r,firstPlaying.time);
        check(ApplyOperation.run(r,firstPlaying,firstPlaying).applied); // Actual new proof, not eligibility, establishes applied.
        Fake targetOnly=new Fake();targetOnly.snapshot=firstPlaying.snapshot;targetOnly.proof=proof(r,targetOnly.time);targetOnly.proof.profile=0;
        check(!ApplyOperation.run(r,targetOnly,targetOnly).applied);

        for(int rate:new int[]{48000,96000}) {
            Fake unsupported=new Fake();unsupported.snapshot=snapshot(true,rate,rate);
            result=ApplyOperation.run(r,unsupported,unsupported);
            check(result.phase==ApplyOperation.Phase.REJECTED && !result.applied);
            check(unsupported.arms==0 && unsupported.recreates==0 && unsupported.restores==0);
        }
        Fake notQualified=new Fake();notQualified.qualified=false;
        check(ApplyOperation.run(r,notQualified,notQualified).phase==ApplyOperation.Phase.REJECTED);
        check(notQualified.arms==0 && notQualified.recreates==0);

        Fake manual=new Fake();manual.changedAfterRecreate=true;
        result=ApplyOperation.run(r,manual,manual);
        check(result.phase==ApplyOperation.Phase.SUPERSEDED && !result.applied);
        check(manual.recoveries==1 && !manual.mayRestore && manual.restores==0);
        Fake changedConfig=new Fake();changedConfig.configAfterProof=true;changedConfig.proof=proof(r,changedConfig.time);
        result=ApplyOperation.run(r,changedConfig,changedConfig);
        check(result.phase==ApplyOperation.Phase.SUPERSEDED && !result.applied);
        check(changedConfig.recoveries==1 && !changedConfig.mayRestore && changedConfig.finishes==0);
        Fake rootLoss=new Fake();rootLoss.loseAuthorityAfterRecreate=true;
        check(ApplyOperation.run(r,rootLoss,rootLoss).phase==ApplyOperation.Phase.SUPERSEDED);
        check(rootLoss.recoveries==1 && !rootLoss.mayRestore);

        Fake partial=new Fake();partial.throwRecreate=true;
        check(ApplyOperation.run(r,partial,partial).phase==ApplyOperation.Phase.RECOVERED);
        check(partial.recoveries==1 && partial.mayRestore && partial.restores==0);
        Fake partialArm=new Fake();partialArm.throwArm=true;
        check(ApplyOperation.run(r,partialArm,partialArm).phase==ApplyOperation.Phase.RECOVERED);
        check(partialArm.recoveries==1 && partialArm.recreates==0 && partialArm.restores==0);
        Fake unknownRecovery=new Fake();unknownRecovery.throwRecreate=true;unknownRecovery.recoveryOkay=false;
        check(ApplyOperation.run(r,unknownRecovery,unknownRecovery).phase==ApplyOperation.Phase.FAILED);
        Fake deadline=new Fake();deadline.expireDuringRecreate=true;
        check(ApplyOperation.run(r,deadline,deadline).phase==ApplyOperation.Phase.RECOVERED);
        check(deadline.recoveries==1 && deadline.finishes==0);

        Fake noProof=new Fake();
        result=ApplyOperation.run(r,noProof,noProof);
        check(result.phase==ApplyOperation.Phase.RECOVERED && !result.applied);
        check(noProof.time==1000+ApplyOperation.OBSERVATION_MILLIS && noProof.recoveries==1);

        ApplyOperation.Snapshot s=snapshot(true,44100,44100);
        ApplyOperation.Proof p=proof(r,1000);check(p.applied(r,s,1000));
        p.epoch=s.epoch;check(!p.applied(r,s,1000));
        p.epoch=epoch(41,"100",4);check(p.applied(r,s,1000));
        p.epoch=epoch(41,"101",3);check(p.applied(r,s,1000));
        p.epoch=new ApplyOperation.Epoch("055c0fe3-d91c-44ab-8700-5220f57d0d66",COOKIE,42,"200",1);check(!p.applied(r,s,1000));
        p.epoch=new ApplyOperation.Epoch(BOOT,"1123456789abcdef0123456789abcdef",42,"200",1);check(!p.applied(r,s,1000));
        for(int condition=0;condition<15;condition++) {
            p=proof(r,1000);
            switch(condition) {
            case 0:p.confirmed=0;break;case 1:p.wr=0;break;case 2:p.deltaUs=149000;break;
            case 3:p.afterUs++;break;case 4:p.sampleRate=96000;break;case 5:p.profile=2;break;
            case 6:p.leaseValid=false;break;case 7:p.error=1;break;case 8:p.phase=7;break;
            case 9:p.authenticated=false;break;case 10:p.open=false;break;case 11:p.targetSg=0;break;
            case 12:p.operationId="fd22fba8-41da-4c3b-baf7-15350e1acc7e";break;
            case 13:p.snapshotToken="1"+HASH.substring(1);break;case 14:p.configIdentity="1"+HASH.substring(1);break;
            }
            check(!p.applied(r,s,1000));
        }
        p=proof(r,1000);check(!p.applied(r,s,3000));check(!p.applied(r,s,999));
        p.providerRevision=2;p.epoch=s.epoch;check(!p.applied(r,s,1000)); // Publisher revision is not a new playback epoch.
        ApplyOperation.Request zero=new ApplyOperation.Request(ID,0,0);p=proof(zero,1000);p.confirmed=0;
        check(!p.applied(zero,s,1000)); // Production zero bypasses the relay and is not packet-application proof.
        Fake disable=new Fake();
        disable.snapshot=new ApplyOperation.Snapshot(s.epoch,HASH,HASH,false,false,false,96000,0,0,true);
        ApplyOperation.DisabledReceipt receipt=new ApplyOperation.DisabledReceipt();receipt.normalSession=epoch(55,"300",0);
        receipt.operationId=ID;receipt.snapshotToken=HASH;receipt.configIdentity=HASH;receipt.observedAt=1000;
        receipt.authenticated=receipt.normalOwnerVerified=receipt.oldRelayRemoved=receipt.stockGslVerified=receipt.currentConfigMatches=true;
        disable.disabledReceipt=receipt;
        result=ApplyOperation.run(zero,disable,disable);
        check(result.phase==ApplyOperation.Phase.DISABLED && !result.applied);
        check(disable.disables==1 && disable.recreates==0 && !disable.resume && disable.observations==0);
        check(receipt.valid(zero,disable.snapshot,1000));
        check(!receipt.valid(zero,disable.snapshot,3000));
        check(!receipt.valid(zero,disable.snapshot,999));
        receipt.normalSession=s.epoch;check(!receipt.valid(zero,disable.snapshot,1000));
        receipt.normalSession=epoch(55,"300",0);receipt.oldRelayRemoved=false;
        check(!receipt.valid(zero,disable.snapshot,1000));
        Fake noReceipt=new Fake();noReceipt.snapshot=disable.snapshot;
        result=ApplyOperation.run(zero,noReceipt,noReceipt);
        check(result.phase==ApplyOperation.Phase.RECOVERED && !result.applied && noReceipt.observations==0);
        System.out.println("Delay apply operation: "+checks+" state/evidence checks passed.");
    }
}
