// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.CRC32;

/** Read-only evidence model. Every byte/helper result comes from the fixed installed source. */
final class RootFacts {
    enum Command { CONFIG,RUNTIME,RECOVERY,PROVIDER,SELECTION,NORMAL,PREFLIGHT }
    interface Source {
        String boot()throws Exception;
        boolean authority()throws Exception;
        long now();
        int hal()throws Exception;
        String start(int pid)throws Exception;
        boolean rootProcess(int pid)throws Exception;
        boolean exists(String modulePath)throws Exception;
        byte[] file(String modulePath,int maximum,boolean privateFile)throws Exception;
        String hash(String modulePath)throws Exception;
        byte[] config()throws Exception;
        byte[] runtime(String name,int size)throws Exception;
        String command(Command command,int oldHal)throws Exception;
        String packageHash()throws Exception;
        String activationHash()throws Exception;
        String currentOperation()throws Exception;
    }
    static final class Context {
        ApplicationInputs.Config config;ApplyOperation.Epoch epoch;
        boolean authority,recovery,reviewedHq,targetEligible,normalRemovalPossible,selectionKnown,selectionUnique;
        int observedRate,profile,selectedRate;
        String packageHash,activationHash,peerHash,codecHash;
        Lease lease;Status status;
    }
    static final class Lease {
        String boot,cookie,start,ownerStart;int hal,owner,hq,ll;long revision,expiry;
    }
    static final class Status {
        String boot,cookie;int hal,owner,stage,error,profile,rate,mappings;long epoch,revision,rd,wr,rewrites,delta,before,after,sg,container,port,observed;
        boolean lease,init,open,start,worker,registered;
    }
    private final Source source;private final String boot,cookie;private final String identity;
    RootFacts(Source source)throws Exception {
        this.source=source;boot=source.boot();if(!boot.matches(ApplicationInputs.UUID))throw new IOException("Invalid current boot");
        String text=ApplicationInputs.utf8(source.file("latency-runtime.identity",230,false));
        if(!text.matches("1 [a-f0-9]{32} [a-f0-9]{64} [a-f0-9]{64} [a-f0-9]{64}\n"))throw new IOException("Invalid packaged runtime identity");
        String[] f=text.trim().split(" ");cookie=f[1];if(cookie.equals("00000000000000000000000000000000"))throw new IOException("Empty cookie");
        for(int i=0;i<3;i++)if(!f[2+i].equals(source.hash(new String[]{"payload/patched-libar-gsl.so","payload/patched-libbluetooth_jni.so","payload/patched-libsehbluetooth_audio_session_aidl.so"}[i])))throw new IOException("Runtime payload identity mismatch");
        identity=source.packageHash();
    }
    Context inspect()throws Exception {
        Context c=new Context();c.authority=source.authority()&&boot.equals(source.boot())&&identity.equals(source.packageHash());
        c.packageHash=identity;c.config=ApplicationInputs.config(source.config());
        Map<String,Object> config=json(Command.CONFIG,0);schema(config,2,null);
        Map<String,Object> requested=object(config,"requested");
        if(number(requested,ApplicationInputs.HQ,150)!=c.config.hq||number(requested,ApplicationInputs.LL,100)!=c.config.ll||bool(config,"applied"))throw new IOException("Config helper and private snapshot disagree");
        int hal=source.hal();String ticks=source.start(hal);
        if(hal<=1||ticks==null||!ticks.matches("[1-9][0-9]{0,18}"))throw new IOException("Actual HAL identity unavailable");
        if(!new String(source.file("state/hal.pid",32,false),StandardCharsets.US_ASCII).equals(hal+"\n")||!new String(source.file("state/hal.start",32,false),StandardCharsets.US_ASCII).equals(ticks+"\n"))c.authority=false;
        c.activationHash=source.activationHash();c.epoch=new ApplyOperation.Epoch(boot,cookie,hal,ticks,0);
        Map<String,Object> recovery=json(Command.RECOVERY,0);schema(recovery,1,"additional_ttp");
        c.recovery=bool(recovery,"owned_recovery_required");String phase=string(recovery,"recovery_phase");
        if(c.recovery?!phase.matches("clean|pending|ssr_requested|ssr_offline|ssr_ready|previous_boot|invalid"):!phase.equals("none"))throw new IOException("Invalid recovery phase");
        if(source.exists("state/latency-remote-uncertain"))c.recovery=true;
        boolean active=source.exists("state/latency-runtime.active");
        String runtime=source.command(Command.RUNTIME,0);
        if(active) {
            String record=ApplicationInputs.utf8(source.file("state/latency-runtime.active",256,true));
            if(!record.equals(boot+" "+cookie+" "+source.hash("payload/patched-libar-gsl.so")+" relay\n")||!runtime.equals("latency_runtime=relay; boot="+boot+"\n")||
                !source.exists("state/latency-runtime.request")||!record.equals(ApplicationInputs.utf8(source.file("state/latency-runtime.request",256,true))))c.recovery=true;
            else try {
                Map<String,Object> provider=json(Command.PROVIDER,0);schema(provider,1,"additional_ttp");
                c.lease=lease(source.runtime("provider.bin",128));c.status=status(source.runtime("status.bin",256));
                if(!validLease(c.lease,c.status,c,provider))throw new IOException("Current provider binding unavailable");
                c.epoch=new ApplyOperation.Epoch(boot,cookie,hal,ticks,c.status.epoch);
                c.observedRate=c.status.rate;c.profile=c.status.profile;
                c.reviewedHq=profileObserved(c.status);
                if(string(provider,"status").equals("recovery_required"))c.recovery=true;
            }catch(Exception unavailable){c.lease=null;c.status=null;c.reviewedHq=false;}
        } else if(!runtime.equals("latency_runtime=inactive\n"))throw new IOException("Runtime status disagrees with owned state");
        else if(source.exists("state/latency-runtime.request"))c.recovery=true;
        try {
            Map<String,Object> selection=json(Command.SELECTION,0);schema(selection,1,"application_selection");c.selectionUnique=bool(selection,"unique");
            if(c.selectionUnique) {c.peerHash=string(selection,"peer_sha256");c.codecHash=string(selection,"codec_sha256");if(!c.peerHash.matches(ApplicationInputs.HEX)||!c.codecHash.matches(ApplicationInputs.HEX))throw new IOException("Invalid actual selection fingerprints");c.selectedRate=(int)number(selection,"selected_rate_hz",192000);
                Object mode=selection.get("codec_specific1");if(!(mode instanceof Long))throw new IOException("Actual mode word unavailable");
                c.targetEligible=bool(selection,"target_eligible")&&bool(selection,"capability_compatible")&&number(selection,"codec_type",Integer.MAX_VALUE)==7&&number(selection,"selection_bits",32)==24&&((Long)mode)==4096&&c.selectedRate==44100;}
            c.selectionKnown=true;
        }catch(Exception unknownSelection){c.selectionKnown=false;c.selectionUnique=false;}
        // Absence of a fence is not a removal preflight. This fixed reader only
        // reports actual reviewed loader/DSP/current full-HAL checks.
        try {Map<String,Object> preflight=json(Command.PREFLIGHT,0);schema(preflight,1,"application_preflight");c.normalRemovalPossible=c.authority&&!c.recovery&&bool(preflight,"normal_removal_possible");}
        catch(Exception unavailable){c.normalRemovalPossible=false;}
        // The same fixed preflight verifies current native/HAL/GSL identity and
        // full binder readiness; its absence cannot authenticate an Apply proof.
        if(!c.normalRemovalPossible)c.authority=false;
        // Recheck against changes occurring while fixed helpers ran. A later
        // call can retry; it never extends a stale observation timestamp.
        if(!boot.equals(source.boot())||hal!=source.hal()||!ticks.equals(source.start(hal))||!c.activationHash.equals(source.activationHash())||
            !identity.equals(source.packageHash())||!c.config.hash.equals(ApplicationInputs.config(source.config()).hash))throw new IOException("Root facts changed during read");
        c.authority=c.authority&&source.authority();if(!c.authority){c.normalRemovalPossible=false;c.targetEligible=false;}
        return c;
    }
    boolean requestCurrent(ApplyOperation.Request r,String hash)throws Exception {
        ApplicationInputs.Config c=ApplicationInputs.config(source.config());return source.authority()&&boot.equals(source.boot())&&r.hqMs==c.hq&&r.llMs==c.ll&&(hash==null||hash.equals(c.hash));
    }
    private boolean validLease(Lease l,Status s,Context c,Map<String,Object> p)throws Exception {
        long now=source.now();
        return bool(p,"available")&&bool(p,"fresh")&&bool(p,"lease_valid")&&l.boot.equals(boot)&&l.cookie.equals(cookie)&&l.hal==c.epoch.halPid&&l.start.equals(c.epoch.halStart)&&
            l.owner>1&&l.ownerStart.equals(source.start(l.owner))&&source.rootProcess(l.owner)&&l.expiry>now&&l.expiry-now<=2000&&l.revision>0&&
            l.hq==c.config.hq*1000&&l.ll==c.config.ll*1000&&
            new String(source.file("state/latency-provider.pid",32,false),StandardCharsets.US_ASCII).equals(l.owner+"\n")&&
            new String(source.file("state/latency-provider.start",32,false),StandardCharsets.US_ASCII).equals(l.ownerStart+"\n")&&
            s.boot.equals(boot)&&s.cookie.equals(cookie)&&s.hal==l.hal&&s.owner==l.owner&&s.observed<=now&&now-s.observed<2000&&s.lease&&s.revision>0&&s.revision<=l.revision;
    }
    private ApplicationOwnerRecord owner(ApplyOperation.Request r,ApplyOperation.Snapshot snapshot,Context c)throws Exception {
        if(!r.id.equals(source.currentOperation())||snapshot==null)return null;
        if(!source.exists("state/latency-application/owned."+r.id))return null;
        ApplicationOwnerRecord o=ApplicationOwnerRecord.decode(source.file("state/latency-application/owned."+r.id,1024,true));
        if(!o.id.equals(r.id)||!o.boot.equals(boot)||!o.cookie.equals(cookie)||!o.token.equals(snapshot.token)||!o.config.equals(snapshot.configIdentity)||
            !o.config.equals(c.config.hash)||o.hq!=r.hqMs||o.ll!=r.llMs||o.oldHal!=snapshot.epoch.halPid||!o.oldStart.equals(snapshot.epoch.halStart)||
            !o.packageHash.equals(c.packageHash)||!o.activationHash.equals(c.activationHash)||o.newHal!=c.epoch.halPid||!o.newStart.equals(c.epoch.halStart))return null;
        byte[] capture=source.file("state/latency-application/channel."+r.id+"/capture.bin",8192,true);
        if(!o.captureHash.equals(ApplicationInputs.sha(capture))||!captureMatches(capture,r,snapshot,c))return null;
        return o;
    }
    boolean snapshotCurrent(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception {
        Context c=inspect();ApplicationOwnerRecord o=owner(r,s,c);return c.authority&&!c.recovery&&o!=null&&!o.phase.equals("unconfirmed")&&!o.phase.equals("armed");
    }
    ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot snapshot)throws Exception {
        Context c=inspect();ApplicationOwnerRecord o=owner(r,snapshot,c);
        if(!c.authority||c.recovery||o==null||c.lease==null||c.status==null||!o.phase.matches("ready|waiting|applied")||o.graph!=0&&o.graph!=c.status.epoch)return null;
        Status s=c.status;ApplyOperation.Proof p=new ApplyOperation.Proof();p.operationId=r.id;p.snapshotToken=snapshot.token;p.configIdentity=snapshot.configIdentity;
        p.epoch=c.epoch;p.authenticated=true;p.leaseValid=true;p.observedAt=s.observed;p.leaseUntil=c.lease.expiry;p.providerRevision=s.revision;
        p.rd=s.rd;p.wr=s.wr;p.confirmed=s.rewrites;p.phase=s.stage;p.error=s.error;p.profile=s.profile;p.sampleRate=s.rate;p.mappings=s.mappings;
        p.deltaUs=s.delta;p.beforeUs=s.before;p.afterUs=s.after;p.targetSg=s.sg;p.container=s.container;p.graphSourcePort=s.port;
        p.init=s.init;p.open=s.open;p.start=s.start;p.worker=s.worker;p.registration=s.registered;return p;
    }
    ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot snapshot)throws Exception {
        Context c=inspect();ApplicationOwnerRecord o=owner(r,snapshot,c);
        if(r.hqMs!=0||!c.authority||c.recovery||o==null||!o.phase.equals("disabled")||!normalVerified(c,snapshot))return null;
        if(!requestCurrent(r,snapshot.configIdentity)||source.hal()!=o.newHal||!o.newStart.equals(source.start(o.newHal)))return null;
        ApplyOperation.DisabledReceipt receipt=new ApplyOperation.DisabledReceipt();receipt.operationId=r.id;receipt.snapshotToken=snapshot.token;receipt.configIdentity=snapshot.configIdentity;
        receipt.normalSession=c.epoch;receipt.observedAt=source.now();receipt.authenticated=receipt.normalOwnerVerified=receipt.oldRelayRemoved=receipt.stockGslVerified=receipt.currentConfigMatches=true;return receipt;
    }
    boolean waitingReady(ApplyOperation.Request r,ApplyOperation.Snapshot snapshot)throws Exception {
        Context c=inspect();ApplicationOwnerRecord o=owner(r,snapshot,c);
        return c.authority&&!c.recovery&&o!=null&&o.phase.equals("waiting")&&!snapshot.playing&&c.lease!=null&&c.status!=null&&
            c.status.error==0&&c.status.worker&&c.status.registered&&c.status.stage<=10&&c.status.stage!=7;
    }
    boolean recovered(ApplyOperation.Request r,ApplyOperation.Snapshot snapshot)throws Exception {
        Context c=inspect();if(!c.authority||c.recovery||!r.id.equals(source.currentOperation())||!source.exists("state/latency-application/owned."+r.id))return false;
        ApplicationOwnerRecord o=ApplicationOwnerRecord.decode(source.file("state/latency-application/owned."+r.id,1024,true));
        if(!o.phase.equals("recovered")||!o.id.equals(r.id)||!o.boot.equals(snapshot.epoch.boot)||!o.cookie.equals(snapshot.epoch.cookie)||!o.token.equals(snapshot.token)||!o.config.equals(snapshot.configIdentity)||
            o.hq!=r.hqMs||o.ll!=r.llMs||o.oldHal!=snapshot.epoch.halPid||!o.oldStart.equals(snapshot.epoch.halStart)||!o.packageHash.equals(c.packageHash)||!o.activationHash.equals(c.activationHash)||o.newHal!=c.epoch.halPid||!o.newStart.equals(c.epoch.halStart))return false;
        byte[] bytes=source.file("state/latency-application/channel."+r.id+"/capture.bin",8192,true);ApplicationCapture capture=ApplicationCapture.decode(bytes);
        return o.captureHash.equals(ApplicationInputs.sha(bytes))&&capture.request.id.equals(r.id)&&capture.request.sameValues(r)&&capture.snapshot.token.equals(snapshot.token)&&capture.snapshot.configIdentity.equals(snapshot.configIdentity)&&normalVerified(c,snapshot);
    }
    boolean captureMatches(byte[] bytes,ApplyOperation.Request r,ApplyOperation.Snapshot s,Context context)throws Exception {
        ApplicationCapture capture=ApplicationCapture.decode(bytes);ApplyOperation.Snapshot original=capture.snapshot;
        if(!capture.request.id.equals(r.id)||!capture.request.sameValues(r)||!original.token.equals(s.token)||!original.configIdentity.equals(s.configIdentity)||!original.epoch.sameAuthority(s.epoch)||original.epoch.halPid!=s.epoch.halPid||!original.epoch.halStart.equals(s.epoch.halStart)||original.epoch.graph!=s.epoch.graph||original.selectedRate!=s.selectedRate||original.observedRate!=s.observedRate||original.profile!=s.profile||original.playing!=s.playing||original.reviewedHq!=s.reviewedHq||original.normalRemovalPossible!=s.normalRemovalPossible||original.targetEligible!=s.targetEligible)return false;
        return context.selectionKnown&&(capture.address==null?!context.selectionUnique:context.selectionUnique&&ApplicationInputs.sha(capture.address.toUpperCase(Locale.ROOT).getBytes(StandardCharsets.US_ASCII)).equals(context.peerHash)&&ApplicationInputs.sha(capture.codecParcel).equals(context.codecHash));
    }
    boolean normalVerified(Context c,ApplyOperation.Snapshot snapshot)throws Exception {
        String old=source.start(snapshot.epoch.halPid);
        if(!c.authority||c.recovery||!c.epoch.valid()||!c.epoch.sameAuthority(snapshot.epoch)||c.epoch.halPid==snapshot.epoch.halPid&&c.epoch.halStart.equals(snapshot.epoch.halStart)||
            source.exists("state/latency-runtime.active")||source.exists("state/latency-runtime.request")||old!=null&&old.equals(snapshot.epoch.halStart))return false;
        Map<String,Object> normal=json(Command.NORMAL,snapshot.epoch.halPid);schema(normal,1,"application_normal");
        return bool(normal,"normal_owner_verified")&&bool(normal,"stock_gsl_verified")&&bool(normal,"old_relay_removed")&&number(normal,"hal_pid",Integer.MAX_VALUE)==c.epoch.halPid&&string(normal,"hal_start").equals(c.epoch.halStart)&&
            source.hal()==c.epoch.halPid&&c.epoch.halStart.equals(source.start(c.epoch.halPid));
    }
    private Map<String,Object> json(Command command,int old)throws Exception{return BoundedJson.object(source.command(command,old));}
    static void schema(Map<String,Object> m,int version,String scope)throws IOException {if(number(m,"schema",100)!=version||scope!=null&&!scope.equals(string(m,"scope")))throw new IOException("Unexpected fixed helper schema/scope");}
    @SuppressWarnings("unchecked")static Map<String,Object> object(Map<String,Object> m,String key)throws IOException{Object v=m.get(key);if(!(v instanceof Map))throw new IOException("Missing object "+key);return (Map<String,Object>)v;}
    static String string(Map<String,Object> m,String key)throws IOException{Object v=m.get(key);if(!(v instanceof String))throw new IOException("Missing string "+key);return (String)v;}
    static boolean bool(Map<String,Object> m,String key)throws IOException{Object v=m.get(key);if(!(v instanceof Boolean))throw new IOException("Missing boolean "+key);return (Boolean)v;}
    static long number(Map<String,Object> m,String key,long maximum)throws IOException{Object v=m.get(key);if(!(v instanceof Long)||((Long)v)<0||((Long)v)>maximum)throw new IOException("Missing/bounded integer "+key);return (Long)v;}
    private static long u(byte[] b,int at,int n){long value=0;for(int i=0;i<n;i++)value|=(b[at+i]&255L)<<(i*8);if(n==8&&value<0)throw new IllegalArgumentException("Unsupported overflowing unsigned counter");return value;}
    private static void record(byte[] b,String magic,int size,int crcAt,int reserved)throws IOException {
        if(b.length!=size||!Arrays.equals(Arrays.copyOf(b,8),magic.getBytes(StandardCharsets.ISO_8859_1))||u(b,8,4)!=1||u(b,12,4)!=size)throw new IOException("Invalid runtime record ABI");
        CRC32 crc=new CRC32();crc.update(b,0,crcAt);if(u(b,crcAt,4)!=crc.getValue())throw new IOException("Runtime CRC mismatch");for(int i=reserved;i<crcAt;i++)if(b[i]!=0)throw new IOException("Unknown runtime reserved field");
    }
    private static String boot(byte[] b){String hex=ApplicationInputs.hex(Arrays.copyOfRange(b,16,32));return hex.substring(0,8)+"-"+hex.substring(8,12)+"-"+hex.substring(12,16)+"-"+hex.substring(16,20)+"-"+hex.substring(20);}
    private static boolean bit(byte[] b,int at)throws IOException{long v=u(b,at,4);if(v>1)throw new IOException("Invalid runtime boolean");return v==1;}
    static Lease lease(byte[] b)throws IOException {record(b,"SHZLAT1\0",128,124,96);Lease l=new Lease();l.boot=boot(b);l.cookie=ApplicationInputs.hex(Arrays.copyOfRange(b,32,48));l.revision=u(b,48,8);l.expiry=u(b,56,8)/1000000;l.hq=(int)u(b,64,4);l.ll=(int)u(b,68,4);l.hal=(int)u(b,72,4);l.owner=(int)u(b,76,4);l.start=""+u(b,80,8);l.ownerStart=""+u(b,88,8);if(l.hq<0||l.hq>150000||l.hq%1000!=0||l.ll<0||l.ll>100000||l.ll%1000!=0||l.hal<=1||l.owner<=1)throw new IOException("Invalid runtime lease values");return l;}
    static Status status(byte[] b)throws IOException {record(b,"SHZSTAT1",256,252,232);Status s=new Status();s.boot=boot(b);s.cookie=ApplicationInputs.hex(Arrays.copyOfRange(b,32,48));s.hal=(int)u(b,48,4);s.stage=(int)u(b,52,4);s.error=(int)u(b,56,4);s.lease=bit(b,60);s.revision=u(b,64,8);s.epoch=u(b,72,8);s.delta=u(b,80,8);s.rd=u(b,88,8);s.wr=u(b,96,8);s.mappings=(int)u(b,104,4);s.before=u(b,112,4);s.after=u(b,116,4);s.rate=(int)u(b,120,4);s.profile=(int)u(b,124,4);s.sg=u(b,128,4);s.container=u(b,132,4);s.port=u(b,136,4);s.init=bit(b,140);s.owner=(int)u(b,144,4);s.registered=bit(b,148);s.worker=bit(b,180);s.observed=u(b,208,8)/1000000;s.open=bit(b,216);s.start=bit(b,220);s.rewrites=u(b,224,8);if(u(b,200,8)==0)throw new IOException("Missing runtime generation");return s;}
    private static boolean profileObserved(Status s){return s.stage==5&&s.error==0&&s.lease&&s.init&&s.open&&s.start&&s.worker&&s.registered&&s.profile==1&&s.rate==44100&&s.epoch>0&&s.sg==0xb00000abL&&s.container==0xe00000e6L&&s.port>0&&s.mappings>0&&s.rd>=s.wr&&s.wr>0&&s.rewrites>0&&s.delta>0&&s.delta<=150000&&s.delta%1000==0&&s.before>0&&s.before<=524280&&s.after<=524280&&s.after>=s.before&&s.after-s.before==s.delta;}
}
