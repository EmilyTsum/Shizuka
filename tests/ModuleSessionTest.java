// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;
import java.io.*;
import java.util.*;

public final class ModuleSessionTest {
    static int checks;
    interface Action { void run() throws Exception; }
    static void check(boolean ok) { checks++; if (!ok) throw new AssertionError("module session " + checks); }
    static void rejects(Action action) throws Exception {
        checks++; try { action.run(); } catch (IllegalArgumentException | IOException expected) { return; }
        throw new AssertionError("Unsafe module session accepted at " + checks);
    }
    static Map<String,Object> response() {
        Map<String,Object> r=new HashMap<>();r.put("schema",1);r.put("scope","settings_session");
        r.put("available",true);r.put("companion_uid",10123);
        r.put("socket","shizuka-settings-01234567-89ab-cdef-0123-456789abcdef");return r;
    }
    public static void main(String[] args)throws Exception {
        String[] command=ModuleSession.command(10123);
        check(Arrays.equals(command,new String[]{"/system/bin/su","-c","/data/adb/ksu/bin/busybox timeout -s KILL 15 /data/adb/ksu/bin/busybox sh /data/adb/modules/s25_aptx_adaptive/settings-bootstrap.sh 10123"}));
        check(ModuleSession.command(110123)[2].endsWith(" 110123"));
        check(ModuleSession.TIMEOUT_SECONDS==45 && ModuleSession.MAX_OUTPUT==16384);
        rejects(()->ModuleSession.command(0));rejects(()->ModuleSession.command(9999));rejects(()->ModuleSession.command(-1));
        Map<String,Object> valid=response();check(ModuleSession.socket(valid,10123).equals(valid.get("socket")));
        for(Object schema:new Object[]{null,true,"1",1.0,4294967297L}) {
            Map<String,Object> bad=response();bad.put("schema",schema);rejects(()->ModuleSession.socket(bad,10123));
        }
        for(Object uid:new Object[]{null,"10123",10123.0,10124,0}) {
            Map<String,Object> bad=response();bad.put("companion_uid",uid);rejects(()->ModuleSession.socket(bad,10123));
        }
        for(Object available:new Object[]{null,false,"true",1}) {
            Map<String,Object> bad=response();bad.put("available",available);rejects(()->ModuleSession.socket(bad,10123));
        }
        for(String socket:new String[]{"","shizuka-settings-../foreign", "shizuka-settings-01234567-89ab-cdef-0123-456789abcdef\n", "shizuka-settings-01234567-89ab-cdef-0123-456789abcdeF", "shizuka-settings-------------------------------------"}) {
            Map<String,Object> bad=response();bad.put("socket",socket);rejects(()->ModuleSession.socket(bad,10123));
        }
        Map<String,Object> bad=response();bad.put("scope","other");rejects(()->ModuleSession.socket(bad,10123));
        byte[] exact=new byte[ModuleSession.MAX_OUTPUT];Arrays.fill(exact,(byte)'a');
        check(ModuleSession.readOutput(new ByteArrayInputStream(exact)).length()==exact.length);
        rejects(()->ModuleSession.readOutput(new ByteArrayInputStream(new byte[ModuleSession.MAX_OUTPUT+1])));
        rejects(()->ModuleSession.readOutput(new ByteArrayInputStream(new byte[]{(byte)0xc3,0x28})));
        check(ModuleSession.readOutput(new ByteArrayInputStream("Root denied\n".getBytes("UTF-8"))).equals("Root denied\n"));
        System.out.println("Normal app module session: "+checks+" checks passed.");
    }
}
