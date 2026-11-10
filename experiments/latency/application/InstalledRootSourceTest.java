// SPDX-License-Identifier: GPL-2.0-only
import java.lang.reflect.*;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;

/** Real filesystem checks from the installed reader; no Android service or fixed device path. */
public final class InstalledRootSourceTest {
    static int checks;static Object call(String name,Class<?>[] types,Object...args)throws Exception {
        Method method=InstalledRootSource.class.getDeclaredMethod(name,types);method.setAccessible(true);
        try{return method.invoke(null,args);}catch(InvocationTargetException wrapped){Throwable c=wrapped.getCause();if(c instanceof Exception)throw (Exception)c;throw wrapped;}
    }
    static void check(boolean okay){checks++;if(!okay)throw new AssertionError("installed file check "+checks);}
    interface Checked{void run()throws Exception;}static void rejects(Checked action)throws Exception{try{action.run();throw new AssertionError("unsafe file accepted");}catch(java.io.IOException expected){checks++;}}
    static Object directory(Path path,int uid,boolean privateMode)throws Exception{return call("directory",new Class<?>[]{Path.class,int.class,boolean.class},path,uid,privateMode);}
    static String record(Path path,int uid,int gid,int size)throws Exception{return (String)call("runtimeStamp",new Class<?>[]{Path.class,int.class,int.class,int.class},path,uid,gid,size);}
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("shizuka-installed-files-",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        int uid=((Number)Files.getAttribute(root,"unix:uid")).intValue(),gid=((Number)Files.getAttribute(root,"unix:gid")).intValue();
        try {
            check(directory(root,uid,true)!=null);rejects(()->directory(root,uid+1,true));
            Files.setPosixFilePermissions(root,PosixFilePermissions.fromString("rwxr-xr-x"));check(directory(root,uid,false)!=null);rejects(()->directory(root,uid,true));
            Files.setPosixFilePermissions(root,PosixFilePermissions.fromString("rwxrwxr-x"));rejects(()->directory(root,uid,false));Files.setPosixFilePermissions(root,PosixFilePermissions.fromString("rwx------"));
            Path file=root.resolve("runtime");Files.write(file,new byte[128]);Files.setPosixFilePermissions(file,PosixFilePermissions.fromString("rw-r-----"));
            String first=record(file,uid,gid,128);check(!first.isEmpty());rejects(()->record(file,uid+1,gid,128));rejects(()->record(file,uid,gid+1,128));rejects(()->record(file,uid,gid,256));
            Files.setPosixFilePermissions(file,PosixFilePermissions.fromString("rw-rw----"));rejects(()->record(file,uid,gid,128));Files.setPosixFilePermissions(file,PosixFilePermissions.fromString("rw-r-----"));
            Path link=root.resolve("link");Files.createSymbolicLink(link,file);rejects(()->record(link,uid,gid,128));rejects(()->directory(link,uid,false));Files.delete(link);
            Files.createLink(link,file);rejects(()->record(file,uid,gid,128));Files.delete(link);
            Path newer=root.resolve("newer");Files.write(newer,new byte[128]);Files.setPosixFilePermissions(newer,PosixFilePermissions.fromString("rw-r-----"));Files.move(newer,file,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);check(!first.equals(record(file,uid,gid,128)));
            Path fifo=root.resolve("fifo");Process mkfifo=new ProcessBuilder("mkfifo",fifo.toString()).start();if(mkfifo.waitFor()!=0)throw new AssertionError("fifo fixture failed");Files.setPosixFilePermissions(fifo,PosixFilePermissions.fromString("rw-r-----"));rejects(()->record(fifo,uid,gid,0));
            System.out.println("Installed root reader: "+checks+" actual filesystem owner/mode/link/special-file/identity checks passed.");
        }finally{try(java.util.stream.Stream<Path> files=Files.walk(root)){for(Path p:(Iterable<Path>)files.sorted(java.util.Comparator.reverseOrder())::iterator)Files.deleteIfExists(p);}}
    }
}
