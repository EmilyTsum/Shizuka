// SPDX-License-Identifier: GPL-2.0-only
public final class CodecTargetEligibilityTest {
    static int checks;static void check(boolean v){checks++;if(!v)throw new AssertionError("target eligibility check "+checks);}
    static CodecSnapshotTest.Legacy codec(int type,int rate,int bits,long mode){return new CodecSnapshotTest.Legacy.Builder().setCodecType(type).setSampleRate(rate).setBitsPerSample(bits).setCodecSpecific1(mode).build();}
    public static void main(String[] args)throws Exception {
        Object selected=codec(7,1,2,4096),capability=codec(7,9,3,0);
        CodecTargetEligibility result=CodecTargetEligibility.inspect(selected,new Object[]{capability});check(result.eligible&&result.capabilityCompatible&&result.bits==24&&result.type==7&&result.mode==4096);
        check(CodecTargetEligibility.inspect(selected,java.util.Arrays.asList(capability)).eligible);
        for(Object other:new Object[]{codec(7,8,2,4096),codec(7,1,1,4096),codec(7,1,2,8192),codec(12,1,1,4096),codec(7,1,2,1197),codec(13,1,2,4096)})check(!CodecTargetEligibility.inspect(other,new Object[]{capability}).eligible);
        check(!CodecTargetEligibility.inspect(selected,new Object[]{}).eligible);
        check(!CodecTargetEligibility.inspect(selected,new Object[]{codec(7,8,2,0)}).eligible);
        CodecSnapshotTest.Modern.Builder a=new CodecSnapshotTest.Modern.Builder().setExtendedCodecType(new CodecSnapshotTest.Identity(42));a.setBitsPerSample(2);
        CodecSnapshotTest.Modern.Builder b=new CodecSnapshotTest.Modern.Builder().setExtendedCodecType(new CodecSnapshotTest.Identity(43));b.setBitsPerSample(2);
        check(!CodecTargetEligibility.inspect(a.build(),new Object[]{b.build()}).eligible);
        // No field in the eligibility result is an encoder/INIT/wire observation.
        check(result.eligible&&((CodecSnapshotTest.Legacy)selected).priority==17&&((CodecSnapshotTest.Legacy)selected).channels==1);
        System.out.println("Selected target eligibility: "+checks+" exact tuple/capability/identity checks passed; no observed profile inferred.");
    }
}
