// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.CRC32;

public final class RootFactsTest {
    static int checks;static final String ID=ApplyOperationTest.ID,BOOT=ApplyOperationTest.BOOT,COOKIE=ApplyOperationTest.COOKIE,HASH=ApplyOperationTest.HASH,ACT="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    static void check(boolean value){checks++;if(!value)throw new AssertionError("root fact check "+checks);}
    interface Checked{void run()throws Exception;}static void rejects(Checked action)throws Exception{try{action.run();throw new AssertionError("unsafe acceptance");}catch(IllegalArgumentException|IOException expected){checks++;}}
    static void put(byte[] b,int at,long value,int n){for(int i=0;i<n;i++)b[at+i]=(byte)(value>>>(8*i));}
    static void crc(byte[] b){CRC32 c=new CRC32();c.update(b,0,b.length-4);put(b,b.length-4,c.getValue(),4);}
    static void identity(byte[] b,String magic){System.arraycopy(magic.getBytes(StandardCharsets.ISO_8859_1),0,b,0,8);put(b,8,1,4);put(b,12,b.length,4);String hex=BOOT.replace("-","")+COOKIE;for(int i=0;i<32;i++)b[16+i]=(byte)Integer.parseInt(hex.substring(i*2,i*2+2),16);}
    static byte[] capture(ApplyOperation.Request r,ApplyOperation.Snapshot s,byte[] parcel)throws Exception {
        ByteArrayOutputStream b=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(b);out.write(new byte[]{'S','H','Z','C','A','P','1',0});out.writeInt(2);
        out.writeUTF(r.id);out.writeInt(r.hqMs);out.writeInt(r.llMs);out.writeUTF(s.token);out.writeUTF(s.configIdentity);out.writeUTF(s.epoch.boot);out.writeUTF(s.epoch.cookie);out.writeInt(s.epoch.halPid);out.writeUTF(s.epoch.halStart);out.writeLong(s.epoch.graph);
        out.writeBoolean(s.playing);out.writeBoolean(s.uniqueBondedPeer);out.writeBoolean(s.reviewedHq);out.writeInt(s.selectedRate);out.writeInt(s.observedRate);out.writeInt(s.profile);out.writeBoolean(s.normalRemovalPossible);out.writeBoolean(s.targetEligible);
        out.writeBoolean(true);out.writeUTF("00:11:22:33:44:55");out.writeInt(parcel.length);out.write(parcel);out.writeBoolean(s.playing);if(s.playing){out.writeUTF("example.player");out.writeUTF(HASH);out.writeLong(5);}out.flush();CRC32 crc=new CRC32();crc.update(b.toByteArray());out.writeInt((int)crc.getValue());out.close();return b.toByteArray();
    }
    static final class Fake implements RootFacts.Source {
        byte[] cfg,lease=new byte[128],status=new byte[256],parcel="synthetic exact codec parcel".getBytes(StandardCharsets.US_ASCII),captured;
        String owner,configJson,provider="{\"schema\":1,\"scope\":\"additional_ttp\",\"available\":true,\"fresh\":true,\"lease_valid\":true,\"status\":\"active\"}";
        boolean active=true,recovery,unique=true,authority=true,normal=true,oldAlive,mutateConfig;
        long time=1000;int currentHal=42;String currentStart="200";ApplyOperation.Request request;ApplyOperation.Snapshot snapshot;
        Fake(int hq,int ll)throws Exception {
            cfg=("schema=2\nadaptive_hq_ttp_delta_ms="+hq+"\nadaptive_ll_ttp_delta_ms="+ll+"\n").getBytes(StandardCharsets.US_ASCII);
            request=new ApplyOperation.Request(ID,hq,ll);snapshot=new ApplyOperation.Snapshot(ApplyOperationTest.epoch(41,"100",3),HASH,ApplicationInputs.sha(cfg),hq>0,true,hq>0,hq>0?44100:96000,hq>0?44100:0,hq>0?1:0,true);
            captured=capture(request,snapshot,parcel);configJson="{\"schema\":2,\"requested\":{\"adaptive_hq_ttp_delta_ms\":"+hq+",\"adaptive_ll_ttp_delta_ms\":"+ll+"},\"applied\":false}";
            identity(lease,"SHZLAT1\0");put(lease,48,2,8);put(lease,56,2900000000L,8);put(lease,64,hq*1000,4);put(lease,68,ll*1000,4);put(lease,72,42,4);put(lease,76,55,4);put(lease,80,200,8);put(lease,88,300,8);crc(lease);
            identity(status,"SHZSTAT1");put(status,48,42,4);put(status,52,5,4);put(status,60,1,4);put(status,64,1,8);put(status,72,1,8);put(status,80,hq*1000,8);put(status,88,3,8);put(status,96,2,8);put(status,104,2,4);put(status,108,2,4);put(status,112,167000,4);put(status,116,167000+hq*1000,4);put(status,120,44100,4);put(status,124,1,4);put(status,128,0xb00000abL,4);put(status,132,0xe00000e6L,4);put(status,136,0x2012,4);put(status,140,1,4);put(status,144,55,4);put(status,148,1,4);put(status,180,1,4);put(status,200,1,8);put(status,208,1000000000L,8);put(status,216,1,4);put(status,220,1,4);put(status,224,2,8);crc(status);owner("ready",0);
        }
        void owner(String phase,long graph)throws Exception {owner="1 "+ID+" "+BOOT+" "+COOKIE+" "+HASH+" "+snapshot.configIdentity+" "+request.hqMs+" "+request.llMs+" 123 456 41 100 "+(phase.equals("armed")?"0 0":"42 200")+" "+graph+" "+phase+" "+ApplicationInputs.sha(captured)+" "+HASH+" "+ACT+"\n";}
        void live(int pid,String ticks,long graph){currentHal=pid;currentStart=ticks;put(lease,72,pid,4);put(lease,80,Long.parseLong(ticks),8);put(status,48,pid,4);put(status,72,graph,8);crc(lease);crc(status);}
        public String boot(){return BOOT;}public boolean authority(){return authority;}public long now(){return time;}public int hal(){return currentHal;}
        public String start(int pid){return pid==currentHal?currentStart:pid==55?"300":pid==123?"456":pid==41&&oldAlive?"100":null;}
        public boolean rootProcess(int pid){return pid==55||pid==123;}
        public boolean exists(String path){return path.equals("state/latency-runtime.active")||path.equals("state/latency-runtime.request")?active:path.equals("state/latency-remote-uncertain")?recovery:path.contains("owned.")?owner!=null:false;}
        public byte[] file(String path,int max,boolean priv)throws Exception {
            String result=path.equals("latency-runtime.identity")?"1 "+COOKIE+" "+HASH+" "+HASH+" "+HASH+"\n":path.equals("state/hal.pid")?currentHal+"\n":path.equals("state/hal.start")?currentStart+"\n":path.equals("state/latency-provider.pid")?"55\n":path.equals("state/latency-provider.start")?"300\n":path.equals("state/latency-runtime.active")||path.equals("state/latency-runtime.request")?BOOT+" "+COOKIE+" "+HASH+" relay\n":path.contains("owned.")?owner:null;
            if(path.endsWith("capture.bin"))return captured.clone();if(result==null)throw new FileNotFoundException(path);return result.getBytes(StandardCharsets.US_ASCII);
        }
        public String hash(String name){return HASH;}public byte[] config(){return cfg.clone();}public byte[] runtime(String name,int size){return name.equals("provider.bin")?lease.clone():status.clone();}public String packageHash(){return HASH;}public String activationHash(){return ACT;}public String currentOperation(){return ID;}
        public String command(RootFacts.Command c,int old)throws Exception {
            switch(c){case CONFIG:return configJson;case RUNTIME:return active?"latency_runtime=relay; boot="+BOOT+"\n":"latency_runtime=inactive\n";
            case RECOVERY:return "{\"schema\":1,\"scope\":\"additional_ttp\",\"owned_recovery_required\":"+recovery+",\"recovery_phase\":\""+(recovery?"pending":"none")+"\"}";
            case PROVIDER:return provider;case PREFLIGHT:return "{\"schema\":1,\"scope\":\"application_preflight\",\"normal_removal_possible\":true}";
            case NORMAL:return "{\"schema\":1,\"scope\":\"application_normal\",\"hal_pid\":42,\"hal_start\":\"200\",\"normal_owner_verified\":"+normal+",\"stock_gsl_verified\":true,\"old_relay_removed\":true}";
            case SELECTION:return "{\"schema\":1,\"scope\":\"application_selection\",\"unique\":"+unique+",\"peer_sha256\":\""+ApplicationInputs.sha("00:11:22:33:44:55".getBytes(StandardCharsets.US_ASCII))+"\",\"codec_sha256\":\""+ApplicationInputs.sha(parcel)+"\",\"selected_rate_hz\":"+(request.hqMs>0?44100:96000)+",\"codec_type\":7,\"codec_specific1\":4096,\"selection_bits\":24,\"capability_compatible\":true,\"target_eligible\":"+(request.hqMs>0)+"}";
            default:throw new IOException("unknown");}
        }
    }
    public static void main(String[] args)throws Exception {
        check(BoundedJson.object("{\"schema\":1}").get("schema") instanceof Long);
        for(String bad:new String[]{"{\"schema\":1,\"schema\":2}","{\"schema\":01}","{\"schema\":true}garbage","{\"a\":NaN}","{\"x\":1,}","{\"x\":\"\\uZZZZ\"}"})rejects(()->BoundedJson.object(bad));
        for(String bad:new String[]{"", "schema=1\n", "schema=2\naptx_hq_ttp_delta_ms=150\n", "schema=2\nadaptive_hq_ttp_delta_ms=015\n", "schema=2\nclassic_qhs_host_aware=false\u000b\n", "schema=2\naptx_voice_enabled=maybe\n", "schema=2\nadaptive_priority=1234567\n"})rejects(()->ApplicationInputs.config(bad.getBytes(StandardCharsets.UTF_8)));
        Fake f=new Fake(150,100);RootFacts facts=new RootFacts(f);RootFacts.Context c=facts.inspect();check(c.authority&&c.reviewedHq&&c.observedRate==44100&&c.normalRemovalPossible);
        check(c.targetEligible);
        Fake normalTarget=new Fake(150,0);normalTarget.active=false;RootFacts.Context eligible=new RootFacts(normalTarget).inspect();
        check(eligible.authority&&eligible.targetEligible&&!eligible.reviewedHq&&eligible.profile==0&&eligible.observedRate==0&&eligible.epoch.graph==0);
        ApplyOperation.Proof proof=facts.observe(f.request,f.snapshot);check(proof!=null&&proof.applied(f.request,f.snapshot,f.time));
        check(facts.snapshotCurrent(f.request,f.snapshot));check(facts.requestCurrent(f.request,f.snapshot.configIdentity));
        f.owner("applied",1);check(facts.observe(f.request,f.snapshot).applied(f.request,f.snapshot,f.time));
        f.owner("applied",2);check(facts.observe(f.request,f.snapshot)==null);
        f.owner("ready",0);f.unique=false;check(facts.observe(f.request,f.snapshot)==null);f.unique=true;
        f.owner=f.owner.replace(HASH+" "+f.snapshot.configIdentity,"1"+HASH.substring(1)+" "+f.snapshot.configIdentity);check(facts.observe(f.request,f.snapshot)==null);f.owner("ready",0);
        f.recovery=true;check(facts.observe(f.request,f.snapshot)==null);f.recovery=false;
        f.status[90]^=1;check(facts.observe(f.request,f.snapshot)==null);f.status[90]^=1;
        f.time=3000;check(facts.observe(f.request,f.snapshot)==null);f.time=1000;
        put(f.status,144,56,4);crc(f.status);check(facts.observe(f.request,f.snapshot)==null);put(f.status,144,55,4);crc(f.status);
        put(f.status,224,0,8);crc(f.status);proof=facts.observe(f.request,f.snapshot);check(proof!=null&&!proof.applied(f.request,f.snapshot,f.time));put(f.status,224,2,8);crc(f.status);
        f.provider=f.provider.replace("\"available\":true","\"available\":\"true\"");check(facts.observe(f.request,f.snapshot)==null);
        Fake zero=new Fake(0,0);zero.active=false;zero.owner("disabled",0);RootFacts normal=new RootFacts(zero);
        ApplyOperation.DisabledReceipt receipt=normal.disabled(zero.request,zero.snapshot);check(receipt!=null&&receipt.valid(zero.request,zero.snapshot,zero.time));
        check(!normal.inspect().reviewedHq&&normal.inspect().selectedRate==96000);
        zero.oldAlive=true;check(normal.disabled(zero.request,zero.snapshot)==null);zero.oldAlive=false;
        zero.normal=false;check(normal.disabled(zero.request,zero.snapshot)==null);zero.normal=true;
        zero.recovery=true;check(normal.disabled(zero.request,zero.snapshot)==null);zero.recovery=false;
        zero.owner("ready",0);check(normal.disabled(zero.request,zero.snapshot)==null);
        String good=new Fake(150,100).owner;for(String bad:new String[]{good.trim(),good+"\n",good.replace(" ready "," magic "),good.replace(" 42 200 "," 41 100 "),good.replace(" 150 100 "," 0150 100 ")})rejects(()->new ApplicationOwnerRecord(bad));
        System.out.println("Root facts: "+checks+" config/JSON/actual-record/operation/normal-receipt binding checks passed.");
    }
}
