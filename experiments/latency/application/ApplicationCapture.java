// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.util.*;
import java.util.zip.CRC32;

/** Bounded private capture ABI2; no arbitrary-class deserialization or durable media token. */
final class ApplicationCapture {
    final ApplyOperation.Request request;final ApplyOperation.Snapshot snapshot;
    final String address,mediaPackage,mediaIdentity;final byte[] codecParcel;final long queue;
    private ApplicationCapture(ApplyOperation.Request r,ApplyOperation.Snapshot s,String address,byte[] parcel,String mediaPackage,String mediaIdentity,long queue){this.request=r;this.snapshot=s;this.address=address;this.codecParcel=parcel;this.mediaPackage=mediaPackage;this.mediaIdentity=mediaIdentity;this.queue=queue;}
    private static boolean bit(DataInputStream in)throws IOException{int value=in.readUnsignedByte();if(value>1)throw new IOException("Invalid capture boolean");return value==1;}
    static ApplicationCapture decode(byte[] bytes)throws Exception {
        if(bytes.length<32||bytes.length>8192)throw new IOException("Capture exceeds bound");CRC32 crc=new CRC32();crc.update(bytes,0,bytes.length-4);
        try(DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes))) {
            byte[] magic=new byte[8];in.readFully(magic);if(!Arrays.equals(magic,new byte[]{'S','H','Z','C','A','P','1',0})||in.readInt()!=2)throw new IOException("Unknown capture ABI");
            ApplyOperation.Request request=new ApplyOperation.Request(in.readUTF(),in.readInt(),in.readInt());String token=in.readUTF(),config=in.readUTF();
            ApplyOperation.Epoch epoch=new ApplyOperation.Epoch(in.readUTF(),in.readUTF(),in.readInt(),in.readUTF(),in.readLong());
            boolean playing=bit(in),unique=bit(in),reviewed=bit(in);int selected=in.readInt(),observed=in.readInt(),profile=in.readInt();boolean removal=bit(in),targetEligible=bit(in);
            if(!epoch.valid()||!token.matches(ApplicationInputs.HEX)||!config.matches(ApplicationInputs.HEX)||selected<0||selected>192000||observed<0||observed>192000||profile<0)throw new IOException("Invalid capture snapshot");
            ApplyOperation.Snapshot snapshot=new ApplyOperation.Snapshot(epoch,token,config,playing,unique,reviewed,selected,observed,profile,removal,targetEligible);
            boolean peer=bit(in);String address=null;byte[] parcel=null;
            if(peer){address=in.readUTF();if(!address.matches("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}"))throw new IOException("Invalid private bonded identity");int n=in.readInt();if(n<1||n>4096)throw new IOException("Codec parcel exceeds bound");parcel=new byte[n];in.readFully(parcel);}
            if(peer!=unique||playing&&!peer)throw new IOException("Inconsistent capture peer");
            if(bit(in)!=playing)throw new IOException("Inconsistent play-state capture");String name=null,identity=null;long queue=-1;
            if(playing){name=in.readUTF();identity=in.readUTF();queue=in.readLong();if(name.isEmpty()||name.length()>255||!identity.matches(ApplicationInputs.HEX))throw new IOException("Invalid private media binding");}
            if(in.readInt()!=(int)crc.getValue()||in.read()!=-1)throw new IOException("Capture CRC or length mismatch");
            return new ApplicationCapture(request,snapshot,address,parcel,name,identity,queue);
        }
    }
}
