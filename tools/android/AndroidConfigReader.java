// SPDX-License-Identifier: GPL-2.0-only
import android.system.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

/** Android-only file descriptor checks for a bounded, no-follow configuration read. */
final class AndroidConfigReader {
    static ExtensionConfig read(Path path) throws Exception {
        FileDescriptor fd = Os.open(path.toString(), OsConstants.O_RDONLY | OsConstants.O_CLOEXEC |
                                   OsConstants.O_NOFOLLOW | OsConstants.O_NONBLOCK, 0);
        FileInputStream input = null;
        try {
            StructStat before = Os.fstat(fd);
            if ((before.st_mode & OsConstants.S_IFMT) != OsConstants.S_IFREG || before.st_size > 8192)
                throw new IllegalArgumentException("config is not a bounded regular file");
            input = new FileInputStream(fd);
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (bytes.size() + count > 8192) throw new IllegalArgumentException("config too large");
                bytes.write(buffer, 0, count);
            }
            StructStat after = Os.fstat(fd);
            boolean trusted = android.os.Process.myUid() == 0 &&
                ClassicQhsPolicy.secureConfig(before.st_uid, before.st_mode) &&
                ClassicQhsPolicy.secureConfig(after.st_uid, after.st_mode) &&
                before.st_dev == after.st_dev && before.st_ino == after.st_ino;
            return ExtensionConfig.parse(new String(bytes.toByteArray(), StandardCharsets.UTF_8), trusted);
        } finally {
            if (input != null) input.close(); else Os.close(fd);
        }
    }
}
