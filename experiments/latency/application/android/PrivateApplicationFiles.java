// SPDX-License-Identifier: GPL-2.0-only
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Arrays;

/** Durable immutable private metadata; caller independently authenticates the parent. */
final class PrivateApplicationFiles {
    static void publishOnce(Path target,byte[] value)throws Exception {
        Path directory=target.getParent();Object key=InstalledRootSource.directory(directory,0,true);
        if(Files.exists(target,LinkOption.NOFOLLOW_LINKS)) {
            if(!Arrays.equals(value,LatencyApplyFenceMain.privateRead(target,value.length)))throw new IOException("Immutable private record changed");
            return;
        }
        Path temporary=Files.createTempFile(directory,".owned-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        try {
            try(FileChannel out=FileChannel.open(temporary,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)){ByteBuffer b=ByteBuffer.wrap(value);while(b.hasRemaining())out.write(b);out.force(true);}
            if(!key.equals(InstalledRootSource.directory(directory,0,true)))throw new IOException("Private metadata directory changed");
            Files.move(temporary,target,StandardCopyOption.ATOMIC_MOVE);
            sync(directory);
        }finally{Files.deleteIfExists(temporary);}
    }
    static void sync(Path directory)throws Exception {try(FileChannel p=FileChannel.open(directory,StandardOpenOption.READ)){p.force(true);}}
}
