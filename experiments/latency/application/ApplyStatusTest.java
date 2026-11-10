// SPDX-License-Identifier: GPL-2.0-only
import java.util.Map;

public final class ApplyStatusTest {
    static int checks;
    static void check(boolean okay){checks++;if(!okay)throw new AssertionError("check "+checks);}
    public static void main(String[] args) throws Exception {
        ApplyStore.Record r=new ApplyStore.Record();r.request=new ApplyOperation.Request(ApplyOperationTest.ID,150,100);
        r.phase=ApplyOperation.Phase.APPLIED;r.snapshot=ApplyOperationTest.snapshot(true,44100,44100);r.reason="last confirmed";
        ApplyOperation.Proof proof=ApplyOperationTest.proof(r.request,1000);
        Map<String,Object> current=ApplyStatus.view(r,r.request.id,r.request.id,true,true,false,true,proof,null,1000);
        check(Boolean.TRUE.equals(current.get("applied")) && current.get("phase").equals("applied"));
        check(Boolean.TRUE.equals(current.get("pending_ll")) && Boolean.FALSE.equals(current.get("pending_hq")));
        Map<String,Object> expired=ApplyStatus.view(r,r.request.id,r.request.id,true,true,false,true,proof,null,3000);
        check(Boolean.FALSE.equals(expired.get("applied")) && expired.get("phase").equals("unconfirmed"));
        check(expired.get("last_outcome").equals("applied") && Boolean.TRUE.equals(expired.get("retry_safe")));
        check(Boolean.FALSE.equals(ApplyStatus.view(r,r.request.id,r.request.id,true,true,false,false,proof,null,1000).get("applied")));
        check(Boolean.FALSE.equals(ApplyStatus.view(r,r.request.id,"other",true,true,false,true,proof,null,1000).get("applied")));
        check(Boolean.FALSE.equals(ApplyStatus.view(r,r.request.id,"other",true,true,false,true,proof,null,1000).get("retry_safe")));
        r.phase=ApplyOperation.Phase.WAITING_FOR_PLAYBACK;
        check(Boolean.TRUE.equals(ApplyStatus.view(r,r.request.id,r.request.id,true,true,false,true,proof,null,1000).get("applied")));
        r.phase=ApplyOperation.Phase.UNCONFIRMED;
        check(Boolean.FALSE.equals(ApplyStatus.view(r,r.request.id,r.request.id,true,true,false,true,proof,null,1000).get("retry_safe")));
        r.request=new ApplyOperation.Request(r.request.id,0,0);r.phase=ApplyOperation.Phase.DISABLED;
        r.snapshot=new ApplyOperation.Snapshot(r.snapshot.epoch,ApplyOperationTest.HASH,ApplyOperationTest.HASH,false,false,false,96000,0,0,true);
        ApplyOperation.DisabledReceipt receipt=new ApplyOperation.DisabledReceipt();receipt.operationId=r.request.id;receipt.observedAt=1000;
        receipt.snapshotToken=ApplyOperationTest.HASH;receipt.configIdentity=ApplyOperationTest.HASH;
        receipt.normalSession=ApplyOperationTest.epoch(77,"500",0);
        receipt.authenticated=receipt.normalOwnerVerified=receipt.oldRelayRemoved=receipt.stockGslVerified=receipt.currentConfigMatches=true;
        current=ApplyStatus.view(r,r.request.id,r.request.id,false,false,true,true,null,receipt,1000);
        check(current.get("phase").equals("disabled") && Boolean.TRUE.equals(current.get("disabled")));
        check(Boolean.FALSE.equals(current.get("applied")) && Boolean.FALSE.equals(current.get("lease_valid")));
        check(Boolean.TRUE.equals(current.get("disable_receipt_valid")) && Boolean.TRUE.equals(current.get("new_session")));
        check(ApplyStatus.view(r,r.request.id,r.request.id,false,false,true,true,null,receipt,3000).get("phase").equals("unconfirmed"));
        current=ApplyStatus.view(null,null,null,false,false,false,false,null,null,1000);
        check(current.get("phase").equals("saved_pending") && Boolean.FALSE.equals(current.get("available")));
        System.out.println("Apply status presentation: "+checks+" current/history/disable checks passed.");
    }
}
