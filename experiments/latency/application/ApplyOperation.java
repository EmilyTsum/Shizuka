// SPDX-License-Identifier: GPL-2.0-only
import java.util.Objects;

/** Bounded delay-apply transaction policy. Only an explicitly qualified adapter may mutate. */
final class ApplyOperation {
    static final long MAX_MILLIS = 180000, OBSERVATION_MILLIS = 15000;
    static final int PROFILE_R3_HQ = 1;
    enum Phase { QUEUED, PREFLIGHT, RECONFIGURING, RESTORING, VERIFYING,
        WAITING_FOR_PLAYBACK, APPLIED, DISABLED, SUPERSEDED, REJECTED, RECOVERED, UNCONFIRMED, FAILED }
    enum Fence { SAME, OWN_TRANSIENT, USER_CHANGED, UNKNOWN }
    static final class Request {
        final String id;
        final int hqMs, llMs;
        Request(String id, int hqMs, int llMs) {
            if (id == null || !id.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}"))
                throw new IllegalArgumentException("Invalid operation UUID");
            if (hqMs < 0 || hqMs > 150 || llMs < 0 || llMs > 100)
                throw new IllegalArgumentException("Delay request is outside its typed domain");
            this.id=id;this.hqMs=hqMs;this.llMs=llMs;
        }
        boolean sameValues(Request other) { return other != null && hqMs==other.hqMs && llMs==other.llMs; }
    }
    static final class Epoch {
        final String boot, cookie, halStart;
        final int halPid;
        final long graph;
        Epoch(String boot, String cookie, int halPid, String halStart, long graph) {
            this.boot=boot;this.cookie=cookie;this.halPid=halPid;this.halStart=halStart;this.graph=graph;
        }
        boolean sameAuthority(Epoch other) {
            return other!=null && Objects.equals(boot,other.boot) && Objects.equals(cookie,other.cookie);
        }
        boolean valid() {
            return boot!=null && boot.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}") &&
                cookie!=null && cookie.matches("[a-f0-9]{32}") && !cookie.equals("00000000000000000000000000000000") &&
                halPid>1 && halStart!=null && halStart.matches("[1-9][0-9]*") && graph>=0;
        }
        boolean newPlaybackEpoch(Epoch before) {
            return valid() && before!=null && before.valid() && sameAuthority(before) && graph>0 &&
                (halPid!=before.halPid || !halStart.equals(before.halStart) || graph!=before.graph);
        }
    }
    static final class Snapshot {
        final Epoch epoch;
        final String token, configIdentity;
        final boolean playing, uniqueBondedPeer, reviewedHq, normalRemovalPossible, targetEligible;
        final int selectedRate, observedRate, profile;
        Snapshot(Epoch epoch,String token,String configIdentity,boolean playing,boolean uniqueBondedPeer,
                 boolean reviewedHq,int selectedRate,int observedRate,int profile) {
            this(epoch,token,configIdentity,playing,uniqueBondedPeer,reviewedHq,selectedRate,observedRate,profile,false);
        }
        Snapshot(Epoch epoch,String token,String configIdentity,boolean playing,boolean uniqueBondedPeer,
                 boolean reviewedHq,int selectedRate,int observedRate,int profile,boolean normalRemovalPossible) {
            this(epoch,token,configIdentity,playing,uniqueBondedPeer,reviewedHq,selectedRate,observedRate,profile,normalRemovalPossible,reviewedHq);
        }
        Snapshot(Epoch epoch,String token,String configIdentity,boolean playing,boolean uniqueBondedPeer,
                 boolean profileObserved,int selectedRate,int observedRate,int profile,boolean normalRemovalPossible,boolean targetEligible) {
            this.epoch=epoch;this.token=token;this.configIdentity=configIdentity;this.playing=playing;
            this.uniqueBondedPeer=uniqueBondedPeer;this.reviewedHq=profileObserved;
            this.selectedRate=selectedRate;this.observedRate=observedRate;this.profile=profile;
            this.normalRemovalPossible=normalRemovalPossible;
            this.targetEligible=targetEligible;
        }
        private boolean valid() {
            return epoch!=null && epoch.valid() && token!=null && token.matches("[a-f0-9]{64}") &&
                configIdentity!=null && configIdentity.matches("[a-f0-9]{64}");
        }
        // Eligibility is permission to attempt this target, never old DSP proof.
        boolean supported() {return valid() && uniqueBondedPeer && targetEligible && selectedRate==44100;}
        boolean canDisable() {return valid() && normalRemovalPossible && (!playing || uniqueBondedPeer);}
    }
    static final class Proof {
        Epoch epoch;
        String operationId,snapshotToken,configIdentity;
        boolean authenticated, leaseValid, init, open, start, worker, registration;
        long observedAt, leaseUntil, providerRevision, rd, wr, confirmed;
        int phase, error, profile, sampleRate, mappings;
        long deltaUs, beforeUs, afterUs, targetSg, container, graphSourcePort;
        boolean applied(Request request, Snapshot snapshot, long now) {
            return request.hqMs>0 && snapshot!=null && snapshot.supported() && request.id.equals(operationId) &&
                snapshot.token.equals(snapshotToken) && snapshot.configIdentity.equals(configIdentity) && epoch!=null && epoch.newPlaybackEpoch(snapshot.epoch) &&
                authenticated && leaseValid && now>=observedAt && now-observedAt<2000 && leaseUntil>now && leaseUntil-now<=2000 &&
                providerRevision>0 && phase==5 && error==0 && init && open && start && worker && registration &&
                profile==PROFILE_R3_HQ && sampleRate==44100 && targetSg==0xb00000abL && container==0xe00000e6L &&
                graphSourcePort>0 && graphSourcePort<=0xffffffffL && mappings>0 && rd>=wr && wr>0 && confirmed>=0 &&
                deltaUs==request.hqMs*1000L && (deltaUs==0 || confirmed>0) &&
                beforeUs>0 && beforeUs<=524280 && afterUs<=524280 && afterUs>=beforeUs && afterUs-beforeUs==deltaUs;
        }
    }
    static final class DisabledReceipt {
        Epoch normalSession;
        String operationId,snapshotToken,configIdentity;
        long observedAt;
        boolean authenticated, normalOwnerVerified, oldRelayRemoved, stockGslVerified, currentConfigMatches;
        boolean valid(Request request,Snapshot snapshot,long now) {
            return request.hqMs==0 && snapshot!=null && snapshot.canDisable() && request.id.equals(operationId) &&
                snapshot.token.equals(snapshotToken) && snapshot.configIdentity.equals(configIdentity) &&
                authenticated && normalOwnerVerified && oldRelayRemoved && stockGslVerified && currentConfigMatches &&
                now>=observedAt && now-observedAt<2000 && normalSession!=null && normalSession.valid() &&
                normalSession.sameAuthority(snapshot.epoch) &&
                (normalSession.halPid!=snapshot.epoch.halPid || !normalSession.halStart.equals(snapshot.epoch.halStart));
        }
    }
    static final class Result {
        final Phase phase;
        final boolean applied, recoveryOkay;
        final String reason;
        Result(Phase phase,boolean applied,boolean recoveryOkay,String reason) {
            this.phase=phase;this.applied=applied;this.recoveryOkay=recoveryOkay;this.reason=reason;
        }
    }
    interface Journal {
        void publish(Request request, Phase phase, Snapshot snapshot, String reason) throws Exception;
    }
    interface Adapter {
        long now();
        boolean qualified() throws Exception;
        default boolean disableQualified() throws Exception {return false;}
        /** Read-only current-session preflight; must not capture/arm restoration state. */
        default boolean currentContextSupported() throws Exception {return false;}
        /** Persisted remote uncertainty is not cleared by a new HAL PID or expired lease. */
        default boolean ownedRecoveryRequired() throws Exception {return false;}
        boolean authority() throws Exception;
        boolean requestCurrent(Request request, Snapshot snapshot) throws Exception;
        Snapshot capture(Request request) throws Exception;
        Fence fence(Snapshot snapshot) throws Exception;
        void armRecovery(Request request,Snapshot snapshot,long deadline) throws Exception;
        void recreate(Request request,Snapshot snapshot,long deadline) throws Exception;
        default void disable(Request request,Snapshot snapshot,long deadline) throws Exception {throw new UnsupportedOperationException("Owned normal removal is unavailable");}
        void restore(Request request,Snapshot snapshot,boolean resumeOriginalOwner,long deadline) throws Exception;
        Proof observe(Request request,Snapshot snapshot) throws Exception;
        default DisabledReceipt observeDisabled(Request request,Snapshot snapshot) throws Exception {return null;}
        void finish(Request request,Snapshot snapshot,boolean waitingForPlayback,long deadline) throws Exception;
        boolean recover(Request request,Snapshot snapshot,boolean mayRestoreUserState,long deadline) throws Exception;
        void pause(long millis) throws Exception;
    }
    private static final class Changed extends Exception { Changed(String text) { super(text); } }
    private static void current(Adapter adapter,Request request,Snapshot snapshot,boolean allowOwnTransient) throws Exception {
        if (!adapter.authority() || adapter.ownedRecoveryRequired()) throw new Changed("Module, root, boot or owned recovery authority changed");
        if (!adapter.requestCurrent(request,snapshot)) throw new Changed("A newer delay configuration was saved");
        if (snapshot!=null) {
            Fence fence=adapter.fence(snapshot);
            if (fence!=Fence.SAME && !(allowOwnTransient && fence==Fence.OWN_TRANSIENT))
                throw new Changed(fence==Fence.USER_CHANGED ? "Playback selection or user intent changed" : "Playback ownership became uncertain");
        }
    }
    static Result run(Request request,Adapter adapter,Journal journal) throws Exception {
        long began=adapter.now(), deadline=began+MAX_MILLIS;
        if (began<0 || deadline<began) throw new IllegalStateException("Invalid monotonic clock");
        Snapshot snapshot=null;
        boolean armed=false, mutationStarted=false;
        try {
            journal.publish(request,Phase.PREFLIGHT,null,"Validating current request and session");
            boolean disabling=request.hqMs==0;
            if (!(disabling?adapter.disableQualified():adapter.qualified())) return end(request,journal,Phase.REJECTED,null,false,false,"This packaged lifecycle executor is not qualified");
            current(adapter,request,null,false);
            snapshot=adapter.capture(request);
            if (snapshot==null || !(disabling?snapshot.canDisable():snapshot.supported()))
                return end(request,journal,Phase.REJECTED,snapshot,false,false,disabling?
                    "Owned normal-route removal is not verified for the current session":"The actual selection is not eligible for the reviewed Adaptive HQ 44.1 kHz/24-bit target");
            current(adapter,request,snapshot,false);
            checkTime(adapter,deadline);
            armed=true; // A partially failing arm can still leave owned recovery intent.
            adapter.armRecovery(request,snapshot,deadline);
            current(adapter,request,snapshot,false);
            journal.publish(request,Phase.RECONFIGURING,snapshot,disabling?"Removing the owned delay route":"Creating a new owned audio epoch");
            mutationStarted=true; // Covers a partially failing recreate call.
            if(disabling)adapter.disable(request,snapshot,deadline);else adapter.recreate(request,snapshot,deadline);
            checkTime(adapter,deadline);current(adapter,request,snapshot,true);
            journal.publish(request,Phase.RESTORING,snapshot,"Restoring only the captured unchanged session");
            adapter.restore(request,snapshot,snapshot.playing,deadline);
            checkTime(adapter,deadline);current(adapter,request,snapshot,false);
            if(disabling) {
                DisabledReceipt receipt=adapter.observeDisabled(request,snapshot);
                if(receipt!=null && receipt.valid(request,snapshot,adapter.now())) {
                    current(adapter,request,snapshot,false);adapter.finish(request,snapshot,false,deadline);
                    return end(request,journal,Phase.DISABLED,snapshot,false,false,"Owned delay route removed; fresh normal session verified");
                }
                boolean recovered=recover(adapter,request,snapshot,true,deadline);
                return end(request,journal,recovered?Phase.RECOVERED:Phase.UNCONFIRMED,snapshot,false,recovered,
                    "No fresh verified normal-owner removal receipt; disabled state remains unconfirmed");
            }
            if (!snapshot.playing) {
                adapter.finish(request,snapshot,true,deadline);
                return end(request,journal,Phase.WAITING_FOR_PLAYBACK,snapshot,false,false,"Ready for next playback; idle state was preserved");
            }
            journal.publish(request,Phase.VERIFYING,snapshot,"Waiting for a fresh completed-buffer TTP observation");
            long observationEnd=Math.min(deadline,adapter.now()+OBSERVATION_MILLIS);
            while (adapter.now()<observationEnd) {
                current(adapter,request,snapshot,false);
                Proof proof=adapter.observe(request,snapshot);
                if (proof!=null && proof.applied(request,snapshot,adapter.now())) {
                    current(adapter,request,snapshot,false); // A concurrent save cannot turn old evidence into current success.
                    adapter.finish(request,snapshot,false,deadline);
                    return end(request,journal,Phase.APPLIED,snapshot,true,false,"Matching delay observed on a new playback epoch");
                }
                adapter.pause(Math.min(250,Math.max(1,observationEnd-adapter.now())));
            }
            boolean recovered=recover(adapter,request,snapshot,true,deadline);
            return end(request,journal,recovered?Phase.RECOVERED:Phase.UNCONFIRMED,snapshot,false,recovered,
                "No fresh matching WR_DONE proof; application remains unconfirmed");
        } catch (Changed changed) {
            boolean recovered=!armed || recover(adapter,request,snapshot,false,deadline);
            return end(request,journal,recovered?Phase.SUPERSEDED:Phase.FAILED,snapshot,false,armed&&recovered,changed.getMessage());
        } catch (Exception failure) {
            boolean recovered=!armed || recover(adapter,request,snapshot,!mutationStarted || unchanged(adapter,request,snapshot),deadline);
            return end(request,journal,recovered && armed?Phase.RECOVERED:Phase.FAILED,snapshot,false,armed&&recovered,
                failure.getClass().getSimpleName()+": "+String.valueOf(failure.getMessage()));
        }
    }
    private static boolean unchanged(Adapter adapter,Request request,Snapshot snapshot) {
        try { current(adapter,request,snapshot,true);return true; } catch(Exception changed) { return false; }
    }
    private static boolean recover(Adapter adapter,Request request,Snapshot snapshot,boolean restore,long deadline) {
        try { return adapter.recover(request,snapshot,restore,Math.max(deadline,adapter.now()+30000)); }
        catch(Exception unknown) { return false; }
    }
    private static void checkTime(Adapter adapter,long deadline) {
        if (adapter.now()>=deadline) throw new IllegalStateException("Delay application deadline expired");
    }
    private static Result end(Request request,Journal journal,Phase phase,Snapshot snapshot,boolean applied,boolean recovered,String reason) throws Exception {
        journal.publish(request,phase,snapshot,reason);
        return new Result(phase,applied,recovered,reason);
    }
    static boolean terminal(Phase phase) {
        return phase==Phase.WAITING_FOR_PLAYBACK || phase==Phase.APPLIED || phase==Phase.DISABLED || phase==Phase.SUPERSEDED ||
            phase==Phase.REJECTED || phase==Phase.RECOVERED || phase==Phase.UNCONFIRMED || phase==Phase.FAILED;
    }
}
