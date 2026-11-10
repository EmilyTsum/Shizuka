// SPDX-License-Identifier: GPL-2.0-only
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.nio.file.attribute.*;
import java.util.Arrays;
import java.util.zip.CRC32;

/** Private atomic journal. Mutating callers must hold the existing module lifecycle flock. */
final class ApplyStore {
    static final int MAX_RECORD=8192, MAX_OPERATIONS=256;
    private static final byte[] MAGIC={'S','H','Z','A','P','P','1',0};
    private static final String UUID="[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}";
    private final Path directory;
    private final int owner;
    private final Object directoryKey;
    enum Claim { NEW, REUSED, BUSY, CONFLICT, FULL }
    static final class Record {
        ApplyOperation.Request request;
        ApplyOperation.Phase phase;
        ApplyOperation.Snapshot snapshot;
        String boot,reason="",workerStart="";
        long sequence,created,updated;
        int workerPid;
        boolean unresolved() { return !ApplyOperation.terminal(phase) || phase==ApplyOperation.Phase.FAILED || phase==ApplyOperation.Phase.UNCONFIRMED; }
    }
    ApplyStore(Path directory,int owner) throws IOException {
        this.directory=directory;this.owner=owner;
        Files.createDirectories(directory,PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        directoryKey=check(directory,true).fileKey();
        if(directoryKey==null) throw new IOException("Directory identity unavailable");
    }
    private BasicFileAttributes check(Path path,boolean dir) throws IOException {
        BasicFileAttributes attributes=Files.readAttributes(path,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        if (!(dir?attributes.isDirectory():attributes.isRegularFile()) ||
            ((Number)Files.getAttribute(path,"unix:uid",LinkOption.NOFOLLOW_LINKS)).intValue()!=owner ||
            !Files.getPosixFilePermissions(path,LinkOption.NOFOLLOW_LINKS).equals(PosixFilePermissions.fromString(dir?"rwx------":"rw-------")) ||
            (!dir && ((Number)Files.getAttribute(path,"unix:nlink",LinkOption.NOFOLLOW_LINKS)).longValue()!=1))
            throw new IOException("Unsafe application journal path");
        return attributes;
    }
    private void pinned() throws IOException {
        if(!directoryKey.equals(check(directory,true).fileKey())) throw new IOException("Application journal directory changed");
    }
    private Path file(String id) {
        if(id==null || !id.matches(UUID)) throw new IllegalArgumentException("Invalid operation UUID");
        return directory.resolve(id+".bin");
    }
    private byte[] read(Path path,int limit) throws IOException {
        pinned();BasicFileAttributes before=check(path,false);
        if(before.size()<1 || before.size()>limit) throw new IOException("Application journal size invalid");
        byte[] bytes;
        try(FileChannel channel=FileChannel.open(path,StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS);
            ByteArrayOutputStream output=new ByteArrayOutputStream()) {
            ByteBuffer buffer=ByteBuffer.allocate(1024);
            while(channel.read(buffer)>0) {
                if(output.size()+buffer.position()>limit) throw new IOException("Application journal grew beyond limit");
                output.write(buffer.array(),0,buffer.position());buffer.clear();
            }
            bytes=output.toByteArray();
        }
        BasicFileAttributes after=check(path,false);pinned();
        if(!before.fileKey().equals(after.fileKey()) || before.size()!=after.size() || !before.lastModifiedTime().equals(after.lastModifiedTime()))
            throw new IOException("Application journal changed during read");
        return bytes;
    }
    private void atomic(Path path,byte[] bytes) throws IOException {
        pinned();if(Files.exists(path,LinkOption.NOFOLLOW_LINKS))check(path,false);
        Path temporary=Files.createTempFile(directory,".apply-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            try(FileChannel output=FileChannel.open(temporary,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)) {
                ByteBuffer data=ByteBuffer.wrap(bytes);while(data.hasRemaining())output.write(data);output.force(true);
            }
            pinned();Files.move(temporary,path,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            try(FileChannel parent=FileChannel.open(directory,StandardOpenOption.READ)){parent.force(true);}
        } finally { Files.deleteIfExists(temporary); }
    }
    synchronized Record load(String id) throws IOException {
        pinned();Path path=file(id);if(!Files.exists(path,LinkOption.NOFOLLOW_LINKS))return null;
        Record record=decode(read(path,MAX_RECORD));
        if(!record.request.id.equals(id))throw new IOException("Journal operation identity mismatch");return record;
    }
    synchronized String currentId() throws IOException {
        pinned();Path path=directory.resolve("current");if(!Files.exists(path,LinkOption.NOFOLLOW_LINKS))return null;
        String id=new String(read(path,64),java.nio.charset.StandardCharsets.US_ASCII);
        if(!id.matches(UUID+"\n"))throw new IOException("Invalid current application identity");return id.trim();
    }
    synchronized Record current() throws IOException {
        String id=currentId();if(id==null)return null;Record record=load(id);
        if(record==null)throw new IOException("Current application journal is missing");return record;
    }
    synchronized Claim claim(ApplyOperation.Request request,String boot,long now) throws IOException {
        if(boot==null || !boot.matches(UUID) || now<0)throw new IllegalArgumentException("Invalid boot/clock");
        Record previous=load(request.id);
        if(previous!=null)return previous.boot.equals(boot)&&previous.request.sameValues(request)?Claim.REUSED:Claim.CONFLICT;
        Record active=current();if(active!=null && active.unresolved())return Claim.BUSY;
        int count=0;try(DirectoryStream<Path> paths=Files.newDirectoryStream(directory,"*.bin")) {for(Path path:paths)if(++count>=MAX_OPERATIONS)return Claim.FULL;}
        Record record=new Record();record.request=request;record.boot=boot;record.created=record.updated=now;
        record.sequence=1;record.phase=ApplyOperation.Phase.QUEUED;record.reason="Application queued";
        atomic(file(request.id),encode(record));
        atomic(directory.resolve("current"),(request.id+"\n").getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        return Claim.NEW;
    }
    synchronized void bind(String id,int pid,String start,long now) throws IOException {
        if(pid<=1 || start==null || !start.matches("[1-9][0-9]*"))throw new IllegalArgumentException("Invalid worker identity");
        Record record=owned(id);if(record.phase!=ApplyOperation.Phase.QUEUED)throw new IOException("Application already left its queue");
        if(record.workerPid==pid && record.workerStart.equals(start))return;
        if(record.workerPid!=0)throw new IOException("Application already has a worker");
        record.workerPid=pid;record.workerStart=start;save(record,now);
    }
    synchronized void publish(String id,ApplyOperation.Phase phase,ApplyOperation.Snapshot snapshot,String reason,long now) throws IOException {
        Record record=owned(id);if(ApplyOperation.terminal(record.phase))throw new IOException("Application already finished");
        record.phase=phase;if(snapshot!=null)record.snapshot=snapshot;record.reason=reason;save(record,now);
    }
    /** Caller must prove the old worker is gone while holding the lifecycle flock. */
    synchronized void resolveOwned(String id,boolean recovered,String reason,long now) throws IOException {
        Record record=owned(id);
        if(!record.unresolved())throw new IOException("No unresolved application is owned here");
        record.phase=recovered?ApplyOperation.Phase.RECOVERED:ApplyOperation.Phase.UNCONFIRMED;
        record.reason=reason;save(record,now);
    }
    /** Only independent stock/full-HAL verification under FD9 may call this. */
    synchronized void resolveVerifiedOrphan(String id,String currentBoot,String reason,long now)throws IOException {
        if(currentBoot==null||!currentBoot.matches(UUID)||now<0)throw new IllegalArgumentException("Invalid reconciliation boot/clock");
        Record record=load(id);if(record==null||!record.unresolved())throw new IOException("No unresolved journal to reconcile");
        record.phase=ApplyOperation.Phase.RECOVERED;record.reason=reason;
        // Original timestamps remain in their original boot's monotonic domain.
        // The separate durable receipt records the current boot and clock.
        save(record,record.boot.equals(currentBoot)?now:record.updated);
    }
    private Record owned(String id) throws IOException {
        if(!id.equals(currentId()))throw new IOException("A newer application owns the journal");
        Record record=load(id);if(record==null)throw new IOException("Application journal missing");return record;
    }
    private void save(Record record,long now) throws IOException {
        if(now<record.updated || record.sequence==Long.MAX_VALUE)throw new IOException("Application journal clock/generation invalid");
        record.updated=now;record.sequence++;atomic(file(record.request.id),encode(record));
    }
    private static void text(DataOutputStream output,String value,int maximum) throws IOException {
        if(value==null || value.length()>maximum)throw new IOException("Application text exceeds limit");output.writeUTF(value);
    }
    private static void epoch(DataOutputStream out,ApplyOperation.Epoch e) throws IOException {
        text(out,e.boot,36);text(out,e.cookie,32);out.writeInt(e.halPid);text(out,e.halStart,24);out.writeLong(e.graph);
    }
    private static ApplyOperation.Epoch epoch(DataInputStream in) throws IOException {
        ApplyOperation.Epoch e=new ApplyOperation.Epoch(in.readUTF(),in.readUTF(),in.readInt(),in.readUTF(),in.readLong());
        if(!e.valid())throw new IOException("Invalid application epoch");return e;
    }
    private static boolean bit(DataInputStream in) throws IOException {int value=in.readUnsignedByte();if(value>1)throw new IOException("Invalid application boolean");return value==1;}
    private static byte[] encode(Record r) throws IOException {
        ByteArrayOutputStream buffer=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(buffer);
        out.write(MAGIC);out.writeInt(2);out.writeLong(r.sequence);out.writeLong(r.created);out.writeLong(r.updated);
        text(out,r.request.id,36);out.writeInt(r.request.hqMs);out.writeInt(r.request.llMs);text(out,r.boot,36);
        text(out,r.phase.name(),32);text(out,r.reason,1024);out.writeInt(r.workerPid);text(out,r.workerStart,24);
        out.writeBoolean(r.snapshot!=null);
        if(r.snapshot!=null) {
            ApplyOperation.Snapshot s=r.snapshot;epoch(out,s.epoch);text(out,s.token,64);text(out,s.configIdentity,64);
            out.writeBoolean(s.playing);out.writeBoolean(s.uniqueBondedPeer);out.writeBoolean(s.reviewedHq);
            out.writeInt(s.selectedRate);out.writeInt(s.observedRate);out.writeInt(s.profile);out.writeBoolean(s.normalRemovalPossible);out.writeBoolean(s.targetEligible);
        }
        out.flush();CRC32 crc=new CRC32();byte[] bytes=buffer.toByteArray();crc.update(bytes);out.writeInt((int)crc.getValue());out.close();
        bytes=buffer.toByteArray();if(bytes.length>MAX_RECORD)throw new IOException("Application record exceeds limit");return bytes;
    }
    private static Record decode(byte[] bytes) throws IOException {
        if(bytes.length<16 || bytes.length>MAX_RECORD)throw new IOException("Invalid application journal length");
        CRC32 crc=new CRC32();crc.update(bytes,0,bytes.length-4);
        if(ByteBuffer.wrap(bytes,bytes.length-4,4).getInt()!=(int)crc.getValue())throw new IOException("Application journal CRC mismatch");
        try(DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes,0,bytes.length-4))) {
            byte[] magic=new byte[8];in.readFully(magic);if(!Arrays.equals(magic,MAGIC)||in.readInt()!=2)throw new IOException("Unknown application journal format");
            Record r=new Record();r.sequence=in.readLong();r.created=in.readLong();r.updated=in.readLong();
            r.request=new ApplyOperation.Request(in.readUTF(),in.readInt(),in.readInt());r.boot=in.readUTF();
            r.phase=ApplyOperation.Phase.valueOf(in.readUTF());r.reason=in.readUTF();r.workerPid=in.readInt();r.workerStart=in.readUTF();
            if(bit(in))r.snapshot=new ApplyOperation.Snapshot(epoch(in),in.readUTF(),in.readUTF(),bit(in),bit(in),bit(in),in.readInt(),in.readInt(),in.readInt(),bit(in),bit(in));
            if(in.read()!=-1 || r.sequence<1 || r.created<0 || r.updated<r.created || !r.boot.matches(UUID) || r.reason.length()>1024 ||
                !(r.workerPid==0 && r.workerStart.isEmpty() || r.workerPid>1 && r.workerStart.matches("[1-9][0-9]*")))throw new IOException("Invalid application journal fields");
            if(r.snapshot!=null && (!r.boot.equals(r.snapshot.epoch.boot)||!r.snapshot.token.matches("[a-f0-9]{64}")||!r.snapshot.configIdentity.matches("[a-f0-9]{64}")))
                throw new IOException("Invalid application snapshot binding");
            return r;
        } catch(IllegalArgumentException invalid) {throw new IOException("Unknown application journal value",invalid);}
    }
}
