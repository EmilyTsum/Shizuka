// SPDX-License-Identifier: GPL-2.0-only
import java.nio.ByteBuffer;
import java.nio.charset.*;
import java.security.MessageDigest;
import java.util.*;

/** Native-reader-compatible canonical config gate, reusing the existing typed control policy. */
final class ApplicationInputs {
    static final String UUID="[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}",HEX="[a-f0-9]{64}";
    static final String HQ="adaptive_hq_ttp_delta_ms",LL="adaptive_ll_ttp_delta_ms";
    static final class Config {final String hash;final int hq,ll;Config(String hash,int hq,int ll){this.hash=hash;this.hq=hq;this.ll=ll;}}
    static String utf8(byte[] bytes)throws Exception{return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();}
    static Config config(byte[] bytes)throws Exception {
        if(bytes.length>16384)throw new IllegalArgumentException("Oversized private config");String text=utf8(bytes);
        if(text.indexOf('\0')>=0)throw new IllegalArgumentException("NUL in private config");
        Set<String> allowed=new HashSet<>(Arrays.asList("schema",HQ,LL,"classic_qhs_host_aware","aptx_voice_enabled","adaptive_priority","aptx_hd_priority"));
        Map<String,String> values=new LinkedHashMap<>();
        for(String raw:text.split("[\\r\\n]")){String line=raw.replaceAll("^[ \\t]+|[ \\t]+$","");if(line.isEmpty()||line.startsWith("#"))continue;for(char c:line.toCharArray())if(c<32&&c!='\t')throw new IllegalArgumentException("Invalid config control character");int equal=line.indexOf('=');if(equal<1)throw new IllegalArgumentException("Expected key=value");String key=line.substring(0,equal).replaceAll("^[ \\t]+|[ \\t]+$",""),value=line.substring(equal+1).replaceAll("^[ \\t]+|[ \\t]+$","");if(!allowed.contains(key)||values.put(key,value)!=null)throw new IllegalArgumentException("Unknown/noncanonical/duplicate config key");}
        if(!"2".equals(values.get("schema")))throw new IllegalArgumentException("Canonical schema2 config is required");
        ExtensionConfig parsed=ExtensionConfig.parse(text,true);
        return new Config(sha(bytes),Integer.parseInt(parsed.value(HQ)),Integer.parseInt(parsed.value(LL)));
    }
    static String sha(byte[] bytes)throws Exception{return hex(MessageDigest.getInstance("SHA-256").digest(bytes));}
    static String hex(byte[] bytes){StringBuilder out=new StringBuilder();for(byte b:bytes)out.append(String.format(Locale.ROOT,"%02x",b&255));return out.toString();}
    static long decimal(String value,long maximum){if(value==null||!value.matches("0|[1-9][0-9]{0,18}"))throw new IllegalArgumentException("Noncanonical unsigned value");long n=Long.parseLong(value);if(n>maximum)throw new IllegalArgumentException("Unsigned value exceeds bound");return n;}
}
