// SPDX-License-Identifier: GPL-2.0-only
import android.os.Parcel;
import android.os.Parcelable;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.util.zip.CRC32;

/** Fixed mutation ABI. Root fact APIs remain supplied by the reviewed lifecycle owner. */
final class FixedLifecycleOwner implements ResidentApplyAdapter.Owner {
    interface Facts {
        boolean qualified()throws Exception;
        boolean disableQualified()throws Exception;
        ResidentApplyAdapter.Context inspect()throws Exception;
        boolean requestCurrent(ApplyOperation.Request r,String configIdentity)throws Exception;
        boolean snapshotCurrent(ApplyOperation.Snapshot s)throws Exception;
        ResidentApplyAdapter.ResetPermit resetPermit(ApplyOperation.Request r,ApplyOperation.Snapshot s,long events)throws Exception;
        ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception;
        ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception;
        boolean resolved(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean recovery)throws Exception;
    }
    private final Facts facts;
    private final GuardianChannel channel;
    private final Path directory;
    FixedLifecycleOwner(Path module,String operationId,Facts facts)throws Exception {
        if(!operationId.matches("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}"))throw new IllegalArgumentException("Invalid operation UUID");
        this.facts=facts;directory=module.resolve("state/latency-application/channel."+operationId);
        channel=new GuardianChannel(directory,0);
    }
    public boolean qualified()throws Exception{return facts.qualified();}
    public boolean disableQualified()throws Exception{return facts.disableQualified();}
    public ResidentApplyAdapter.Context inspect()throws Exception{return facts.inspect();}
    public boolean requestCurrent(ApplyOperation.Request r,String config)throws Exception{return facts.requestCurrent(r,config);}
    public boolean snapshotCurrent(ApplyOperation.Snapshot s)throws Exception{return facts.snapshotCurrent(s);}
    public ResidentApplyAdapter.ResetPermit resetPermit(ApplyOperation.Request r,ApplyOperation.Snapshot s,long events)throws Exception{return facts.resetPermit(r,s,events);}
    public ApplyOperation.Proof observe(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return facts.observe(r,s);}
    public ApplyOperation.DisabledReceipt disabled(ApplyOperation.Request r,ApplyOperation.Snapshot s)throws Exception{return facts.disabled(r,s);}
    public void arm(ApplyOperation.Request r,ApplyOperation.Snapshot s,AndroidPlaybackSession.Captured captured,long deadline)throws Exception {
        persistCapture(r,s,captured);
        channel.invoke(GuardianChannel.Action.ARM,r,s,false,deadline);
    }
    public void recreate(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception{channel.invoke(GuardianChannel.Action.RECREATE,r,s,false,deadline);}
    public void remove(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception{channel.invoke(GuardianChannel.Action.REMOVE,r,s,false,deadline);}
    public void finish(ApplyOperation.Request r,ApplyOperation.Snapshot s,boolean waiting,long deadline)throws Exception {
        channel.invoke(GuardianChannel.Action.FINISH,r,s,waiting,deadline);
        if(!facts.resolved(r,s,false))throw new IOException("Lifecycle owner has not verified completion");
    }
    public boolean recover(ApplyOperation.Request r,ApplyOperation.Snapshot s,long deadline)throws Exception {
        channel.invoke(GuardianChannel.Action.RECOVER,r,s,false,deadline);
        return facts.resolved(r,s,true);
    }
    private void persistCapture(ApplyOperation.Request r,ApplyOperation.Snapshot s,AndroidPlaybackSession.Captured c)throws Exception {
        if(c==null)throw new IOException("Missing resident Android capture");
        ByteArrayOutputStream buffer=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(buffer);
        out.write(new byte[]{'S','H','Z','C','A','P','1',0});out.writeInt(2);
        out.writeUTF(r.id);out.writeInt(r.hqMs);out.writeInt(r.llMs);out.writeUTF(s.token);out.writeUTF(s.configIdentity);
        out.writeUTF(s.epoch.boot);out.writeUTF(s.epoch.cookie);out.writeInt(s.epoch.halPid);out.writeUTF(s.epoch.halStart);out.writeLong(s.epoch.graph);
        out.writeBoolean(s.playing);out.writeBoolean(s.uniqueBondedPeer);out.writeBoolean(s.reviewedHq);out.writeInt(s.selectedRate);out.writeInt(s.observedRate);out.writeInt(s.profile);out.writeBoolean(s.normalRemovalPossible);out.writeBoolean(s.targetEligible);
        out.writeBoolean(c.peer!=null);
        if(c.peer!=null) {
            String address=c.peer.getAddress();if(address==null||!address.matches("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}"))throw new IOException("Invalid captured bonded address");
            out.writeUTF(address);
            Object clone=c.codec.cloneObject();
            if(!clone.getClass().getName().equals("android.bluetooth.BluetoothCodecConfig")||!(clone instanceof Parcelable))throw new IOException("Unexpected framework codec class");
            Parcel parcel=Parcel.obtain();byte[] bytes;
            try{((Parcelable)clone).writeToParcel(parcel,0);bytes=parcel.marshall();}finally{parcel.recycle();}
            if(bytes.length<1||bytes.length>4096)throw new IOException("Codec parcel exceeds bound");out.writeInt(bytes.length);out.write(bytes);
        }
        out.writeBoolean(c.playing);
        if(c.playing) {
            String name=c.owner.getPackageName();if(name==null||name.length()>255||c.mediaIdentity==null||!c.mediaIdentity.matches("[a-f0-9]{64}"))throw new IOException("Invalid media owner binding");
            out.writeUTF(name);out.writeUTF(c.mediaIdentity);out.writeLong(c.queueId);
        }
        // The live MediaSession.Token deliberately stays in this process. A dead
        // worker cannot restore playback by deserializing an old token later.
        out.flush();CRC32 crc=new CRC32();crc.update(buffer.toByteArray());out.writeInt((int)crc.getValue());out.close();
        byte[] bytes=buffer.toByteArray();if(bytes.length>8192)throw new IOException("Captured session exceeds bound");
        Path temporary=Files.createTempFile(directory,".capture-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            try(FileChannel output=FileChannel.open(temporary,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)) {
                ByteBuffer data=ByteBuffer.wrap(bytes);while(data.hasRemaining())output.write(data);output.force(true);
            }
            Files.move(temporary,directory.resolve("capture.bin"),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            try(FileChannel parent=FileChannel.open(directory,StandardOpenOption.READ)){parent.force(true);}
        }finally{Files.deleteIfExists(temporary);}
    }
}
