// SPDX-License-Identifier: GPL-2.0-only
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/** Current presentation is recomputed; a stored successful outcome is only history. */
final class ApplyStatus {
    static Map<String,Object> view(ApplyStore.Record record,String requestedId,String currentId,
            boolean available,boolean contextSupported,boolean disableAvailable,boolean configMatches,
            ApplyOperation.Proof proof,ApplyOperation.DisabledReceipt receipt,long now) {
        Map<String,Object> out=new LinkedHashMap<>();
        out.put("schema",1);out.put("scope","latency_application");out.put("available",available);
        out.put("current_context_supported",contextSupported);out.put("disable_available",disableAvailable);
        out.put("target_eligible",contextSupported);out.put("profile_observed",false);
        out.put("operation_known",record!=null);out.put("current_operation_id",currentId);
        out.put("operation_id",record==null?requestedId:record.request.id);out.put("accepted",record!=null);
        out.put("applied",false);out.put("disabled",false);out.put("fresh",false);out.put("lease_valid",false);
        out.put("disable_receipt_valid",false);out.put("new_epoch",false);out.put("new_session",false);
        out.put("normal_owner_verified",false);out.put("old_relay_removed",false);out.put("stock_gsl_verified",false);
        out.put("observation_age_ms",2000);out.put("lease_remaining_ms",0);out.put("active_delta_us",null);
        out.put("graph_epoch","0");out.put("wr_frames",0);out.put("confirmed_rewritten_wrappers",0);
        out.put("phase",requestedId==null?"saved_pending":"unconfirmed");out.put("terminal",true);
        out.put("current_config_matches",false);out.put("pending_hq",true);out.put("pending_ll",true);
        out.put("retry_safe",record==null && requestedId==null);
        out.put("reason",requestedId==null?"Saved values are pending an explicit qualified application":"Operation journal unavailable");
        if(record==null)return out;
        String raw=record.phase.name().toLowerCase(Locale.ROOT);
        out.put("last_outcome",raw);out.put("requested_hq_ms",record.request.hqMs);out.put("requested_ll_ms",record.request.llMs);
        out.put("pending_ll",record.request.llMs!=0);out.put("retry_safe",!record.unresolved());
        out.put("terminal",ApplyOperation.terminal(record.phase));out.put("phase",raw);out.put("reason",record.reason);
        boolean owns=record.request.id.equals(currentId) && configMatches;
        out.put("current_config_matches",owns);
        if(!record.request.id.equals(currentId)) {
            out.put("phase","superseded");out.put("terminal",true);out.put("retry_safe",false);out.put("reason","A newer operation owns the current session");return out;
        }
        if(!owns) {
            out.put("phase","superseded");out.put("terminal",true);out.put("reason","The current saved configuration differs from this operation");return out;
        }
        boolean canConfirm=record.phase==ApplyOperation.Phase.APPLIED || record.phase==ApplyOperation.Phase.WAITING_FOR_PLAYBACK;
        if(available && contextSupported && canConfirm && proof!=null && proof.applied(record.request,record.snapshot,now)) {
            out.put("phase","applied");out.put("terminal",true);out.put("applied",true);out.put("pending_hq",false);
            out.put("profile_observed",true);
            out.put("fresh",true);out.put("lease_valid",true);out.put("new_epoch",true);
            out.put("observation_age_ms",now-proof.observedAt);out.put("lease_remaining_ms",proof.leaseUntil-now);
            out.put("active_delta_us",proof.deltaUs);out.put("graph_epoch",Long.toString(proof.epoch.graph));
            out.put("wr_frames",proof.wr);out.put("confirmed_rewritten_wrappers",proof.confirmed);
            out.put("reason","HQ delay observed on a new reviewed playback epoch; radio latency is not measured");
        } else if(disableAvailable && record.phase==ApplyOperation.Phase.DISABLED && receipt!=null && receipt.valid(record.request,record.snapshot,now)) {
            out.put("phase","disabled");out.put("terminal",true);out.put("disabled",true);out.put("pending_hq",false);
            out.put("fresh",true);out.put("disable_receipt_valid",true);out.put("new_session",true);
            out.put("normal_owner_verified",true);out.put("old_relay_removed",true);out.put("stock_gsl_verified",true);
            out.put("observation_age_ms",now-receipt.observedAt);
            out.put("reason","Owned HQ delay route removed; a fresh normal stock-GSL session is verified");
        } else if(record.phase==ApplyOperation.Phase.APPLIED || record.phase==ApplyOperation.Phase.DISABLED) {
            out.put("phase","unconfirmed");out.put("reason","The last successful outcome has no current fresh verification");
        }
        return out;
    }
}
