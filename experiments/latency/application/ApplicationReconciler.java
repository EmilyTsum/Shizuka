// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.util.*;

/** Independent normal-owner verification. It cannot borrow a live guardian's authority. */
final class ApplicationReconciler {
    interface Probe {String start(int pid)throws Exception;}
    static final class Normal {
        String boot,start,inputsHash;int hal;long observedAt;boolean verified;
    }
    static final class Bundle {
        ApplyStore.Record record;ApplicationCapture capture;byte[] captureBytes,runtimeIdentity;ApplicationOwnerRecord owner;
    }
    static final class Resolution {
        final Bundle bundle;final String originalPackage,captureHash;final boolean newBoot;
        Resolution(Bundle b,String pkg,String capture,boolean freshBoot){bundle=b;originalPackage=pkg;captureHash=capture;newBoot=freshBoot;}
    }
    static List<Resolution> verify(List<Bundle> bundles,String currentBoot,String currentPackage,long now,Normal normal,Probe probe)throws Exception {
        if(!currentBoot.matches(ApplicationInputs.UUID)||!currentPackage.matches(ApplicationInputs.HEX)||normal==null||!normal.verified||!currentBoot.equals(normal.boot)||normal.hal<=1||normal.start==null||!normal.start.matches("[1-9][0-9]{0,18}")||normal.inputsHash==null||!normal.inputsHash.matches(ApplicationInputs.HEX)||now<normal.observedAt||now-normal.observedAt>=2000)throw new IOException("Fresh independently verified stock/full-HAL evidence required");
        List<Resolution> out=new ArrayList<>();
        for(Bundle b:bundles) {
            ApplyStore.Record r=b.record;if(r==null||!r.unresolved())continue;boolean newBoot=!r.boot.equals(currentBoot);
            if(!r.boot.matches(ApplicationInputs.UUID))throw new IOException("Invalid original boot");
            if(!newBoot&&r.workerPid>1&&r.workerStart.equals(probe.start(r.workerPid)))throw new IOException("The original guardian is still alive");
            String captureHash=null,pkg=null;ApplyOperation.Snapshot original=r.snapshot;
            if(b.capture!=null) {
                if(b.captureBytes==null||!b.capture.request.id.equals(r.request.id)||!b.capture.request.sameValues(r.request)||!b.capture.snapshot.epoch.boot.equals(r.boot))throw new IOException("Private capture/journal mismatch");
                captureHash=ApplicationInputs.sha(b.captureBytes);original=b.capture.snapshot;
                if(r.snapshot!=null&&(!r.snapshot.token.equals(original.token)||!r.snapshot.configIdentity.equals(original.configIdentity)||!r.snapshot.epoch.sameAuthority(original.epoch)||r.snapshot.epoch.halPid!=original.epoch.halPid||!r.snapshot.epoch.halStart.equals(original.epoch.halStart)||r.snapshot.epoch.graph!=original.epoch.graph||r.snapshot.playing!=original.playing||r.snapshot.uniqueBondedPeer!=original.uniqueBondedPeer||r.snapshot.reviewedHq!=original.reviewedHq||r.snapshot.targetEligible!=original.targetEligible||r.snapshot.normalRemovalPossible!=original.normalRemovalPossible||r.snapshot.selectedRate!=original.selectedRate||r.snapshot.observedRate!=original.observedRate||r.snapshot.profile!=original.profile))throw new IOException("Journal snapshot changed");
            }
            if(b.owner!=null) {
                ApplicationOwnerRecord o=b.owner;
                if(original==null||captureHash==null||!o.id.equals(r.request.id)||!o.boot.equals(r.boot)||!o.cookie.equals(original.epoch.cookie)||!o.token.equals(original.token)||!o.config.equals(original.configIdentity)||o.hq!=r.request.hqMs||o.ll!=r.request.llMs||o.guardian!=r.workerPid||!o.guardianStart.equals(r.workerStart)||o.oldHal!=original.epoch.halPid||!o.oldStart.equals(original.epoch.halStart)||!o.captureHash.equals(captureHash))throw new IOException("Original package/owner/capture binding unavailable");
                pkg=o.packageHash;
            } else if(r.snapshot!=null)throw new IOException("A mutating-phase journal lost its owner/package receipt");
            // No owner and no published snapshot is a queued/pre-arm operation;
            // the fixed mutator cannot have passed ARM. Still require normal
            // stock evidence and retirement of any captured HAL identity.
            if(!newBoot&&original!=null&&original.epoch.halStart.equals(probe.start(original.epoch.halPid)))throw new IOException("The old HAL identity is still alive");
            if(!newBoot&&original!=null&&normal.hal==original.epoch.halPid&&normal.start.equals(original.epoch.halStart))throw new IOException("No fresh normal HAL identity");
            out.add(new Resolution(b,pkg,captureHash,newBoot));
        }
        return out;
    }
    static Map<String,Object> receipt(Resolution r,String boot,String currentPackage,Normal n) {
        return ControlJson.object("schema",1,"scope","latency_application_reconciled","operation_id",r.bundle.record.request.id,
            "requested_hq_ms",r.bundle.record.request.hqMs,"requested_ll_ms",r.bundle.record.request.llMs,"record_sequence_before",r.bundle.record.sequence,
            "original_boot",r.bundle.record.boot,"current_boot",boot,"original_package_sha256",r.originalPackage,
            "current_package_sha256",currentPackage,"capture_sha256",r.captureHash,"original_phase",r.bundle.record.phase.name(),
            "normal_hal_pid",n.hal,"normal_hal_start",n.start,"observed_monotonic_ms",n.observedAt,"stock_normal_verified",true,
            "mode",r.newBoot?"new_boot":"same_boot");
    }
    static boolean receiptMatches(Map<String,Object> m,ApplyStore.Record r)throws Exception {
        Set<String> keys=new HashSet<>(Arrays.asList("schema","scope","operation_id","requested_hq_ms","requested_ll_ms","record_sequence_before","original_boot","current_boot","original_package_sha256","current_package_sha256","capture_sha256","original_phase","normal_hal_pid","normal_hal_start","observed_monotonic_ms","stock_normal_verified","mode"));
        if(!m.keySet().equals(keys)||!Long.valueOf(1).equals(m.get("schema"))||!"latency_application_reconciled".equals(m.get("scope"))||!r.request.id.equals(m.get("operation_id"))||!r.boot.equals(m.get("original_boot"))||!Long.valueOf(r.request.hqMs).equals(m.get("requested_hq_ms"))||!Long.valueOf(r.request.llMs).equals(m.get("requested_ll_ms"))||!Boolean.TRUE.equals(m.get("stock_normal_verified")))return false;
        String current=string(m,"current_boot",ApplicationInputs.UUID),pkg=string(m,"current_package_sha256",ApplicationInputs.HEX),phase=string(m,"original_phase","[A-Z_]{1,32}");
        for(String key:new String[]{"original_package_sha256","capture_sha256"})if(m.get(key)!=null)string(m,key,ApplicationInputs.HEX);
        String mode=string(m,"mode","same_boot|new_boot");if(mode.equals("same_boot")!=r.boot.equals(current))return false;
        long before=number(m,"record_sequence_before",Long.MAX_VALUE-1);if(before<1||number(m,"normal_hal_pid",Integer.MAX_VALUE)<=1||number(m,"observed_monotonic_ms",Long.MAX_VALUE)<0)return false;
        string(m,"normal_hal_start","[1-9][0-9]{0,18}");
        if(r.phase==ApplyOperation.Phase.RECOVERED)return r.sequence==before+1;
        return r.unresolved()&&r.sequence==before&&r.phase.name().equals(phase);
    }
    private static String string(Map<String,Object> m,String key,String pattern)throws IOException {Object v=m.get(key);if(!(v instanceof String)||!((String)v).matches(pattern))throw new IOException("Invalid reconciliation receipt text");return(String)v;}
    private static long number(Map<String,Object> m,String key,long max)throws IOException {Object v=m.get(key);if(!(v instanceof Long)||(Long)v<0||(Long)v>max)throw new IOException("Invalid reconciliation receipt integer");return(Long)v;}
    /** A stale request may only be archived using the original ARM identity. */
    static Resolution requestOwner(byte[] bytes,List<Resolution> resolutions)throws Exception {
        String request=ApplicationInputs.utf8(bytes);
        if(!request.matches(ApplicationInputs.UUID+" [a-f0-9]{32} [a-f0-9]{64} (relay|observer)\n"))throw new IOException("Noncanonical pending runtime request");
        for(Resolution r:resolutions) {
            Bundle b=r.bundle;if(b.owner==null||b.runtimeIdentity==null)continue;
            String identity=ApplicationInputs.utf8(b.runtimeIdentity);
            if(!identity.matches("1 [a-f0-9]{32} [a-f0-9]{64} [a-f0-9]{64} [a-f0-9]{64}\n"))throw new IOException("Original ARM runtime identity unavailable");
            String[] f=identity.trim().split(" ");if(!f[1].equals(b.owner.cookie))throw new IOException("Original ARM cookie changed");
            if(request.equals(b.owner.boot+" "+f[1]+" "+f[2]+" relay\n"))return r;
        }
        throw new IOException("Pending runtime request belongs to another owner");
    }
}
