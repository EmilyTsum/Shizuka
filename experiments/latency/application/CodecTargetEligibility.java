// SPDX-License-Identifier: GPL-2.0-only
import java.lang.reflect.Array;

/** Selected-target eligibility only. This never reports an observed DSP profile. */
final class CodecTargetEligibility {
    final boolean eligible,capabilityCompatible;final int type,bits;final long mode;
    private CodecTargetEligibility(boolean eligible,boolean capability,int type,int bits,long mode){this.eligible=eligible;this.capabilityCompatible=capability;this.type=type;this.bits=bits;this.mode=mode;}
    private static Object get(Object value,String name)throws Exception{return value.getClass().getMethod(name).invoke(value);}
    static CodecTargetEligibility inspect(Object codec,Object capabilities)throws Exception {
        CodecSnapshot exact=CodecSnapshot.capture(codec);exact.cloneObject();boolean compatible=false;
        if(capabilities instanceof Iterable)for(Object c:(Iterable<?>)capabilities)compatible|=exact.advertisedBy(c);
        else if(capabilities!=null&&capabilities.getClass().isArray())for(int i=0;i<Array.getLength(capabilities);i++)compatible|=exact.advertisedBy(Array.get(capabilities,i));
        else throw new IllegalArgumentException("Actual selectable capabilities unavailable");
        int type=(Integer)get(codec,"getCodecType"),rate=(Integer)get(codec,"getSampleRate"),flag=(Integer)get(codec,"getBitsPerSample");long mode=(Long)get(codec,"getCodecSpecific1");
        int bits=0;for(int depth:new int[]{16,24,32})try{if(codec.getClass().getField("BITS_PER_SAMPLE_"+depth).getInt(null)==flag)bits=depth;}catch(NoSuchFieldException ignored){}
        int rate44100=codec.getClass().getField("SAMPLE_RATE_44100").getInt(null);
        // Private type/mode values apply only after the fixed backend/native
        // pair has independently passed its device/package/live preflight.
        return new CodecTargetEligibility(compatible&&type==7&&mode==4096&&rate==rate44100&&bits==24,compatible,type,bits,mode);
    }
}
