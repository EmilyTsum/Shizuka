// SPDX-License-Identifier: GPL-2.0-only
import java.util.*;
public final class PlaybackSelectionTest {
    static int count;
    static void check(boolean value) { count++; if (!value) throw new AssertionError("check " + count); }
    interface Action { void run() throws Exception; }
    static void rejected(Action action) throws Exception {
        count++; try { action.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("Expected rejection at " + count);
    }
    public static final class CodecType {
        final int id; final String name;
        CodecType(int id, String name) { this.id=id; this.name=name; }
        public String getCodecName() { return name; }
        public boolean equals(Object other) { return other instanceof CodecType && ((CodecType)other).id == id; }
        public int hashCode() { return id; }
    }
    public static final class Codec {
        public static final int SAMPLE_RATE_44100=1, SAMPLE_RATE_48000=2, SAMPLE_RATE_96000=8;
        public static final int BITS_PER_SAMPLE_16=1, BITS_PER_SAMPLE_24=2, BITS_PER_SAMPLE_32=4;
        public static final int CHANNEL_MODE_STEREO=2, CODEC_PRIORITY_HIGHEST=1000000;
        CodecType type; int rate,bits,channel=2,priority; long mode;
        Codec(CodecType type, int rate, int bits) { this.type=type; this.rate=rate; this.bits=bits; }
        public CodecType getExtendedCodecType() { return type; }
        public int getCodecType() { return type.id; }
        public int getSampleRate() { return rate; }
        public int getBitsPerSample() { return bits; }
        public int getChannelMode() { return channel; }
        public long getCodecSpecific1() { return mode; }
        public static class Builder {
            Codec c = new Codec(null,0,0);
            public Builder setExtendedCodecType(CodecType v) { c.type=v; return this; }
            public Builder setCodecPriority(int v) { c.priority=v; return this; }
            public Builder setSampleRate(int v) { c.rate=v; return this; }
            public Builder setBitsPerSample(int v) { c.bits=v; return this; }
            public Builder setChannelMode(int v) { c.channel=v; return this; }
            public Builder setCodecSpecific1(long v) { c.mode=v; return this; }
            public Codec build() { return c; }
        }
    }
    public static final class Status {
        Object items;
        Status(Object items) {this.items=items;}
        public Object getCodecsSelectableCapabilities() {return items;}
    }
    public static void main(String[] ignored) throws Exception {
        check(PlaybackSelection.family("aptX® Adaptive").equals("adaptive"));
        check(PlaybackSelection.family("aptX Lossless").equals("lossless"));
        check(PlaybackSelection.family("aptX LL").equals("other"));
        CodecType type = new CodecType(731,"aptX Adaptive");
        Codec cap = new Codec(type,11,2);
        PlaybackSelection.Request hq = PlaybackSelection.parse(new String[]{"select","active","adaptive","hq","96000","24"});
        Codec wanted=(Codec)PlaybackSelection.build(PlaybackSelection.choose(Arrays.asList(cap),hq),hq);
        check(wanted.type == type && wanted.getCodecType()==731);
        check(wanted.rate==8 && wanted.bits==2 && wanted.mode==4096 && wanted.priority==1000000);
        check(PlaybackSelection.matches(wanted,wanted,hq));
        Codec observed = new Codec(type,8,2); observed.mode=8192;
        check(!PlaybackSelection.matches(observed,wanted,hq));
        rejected(() -> PlaybackSelection.choose(Arrays.asList(cap),new PlaybackSelection.Request("active","adaptive","hq",96000,16)));
        rejected(() -> PlaybackSelection.choose(Arrays.asList(new Codec(type,2,2),new Codec(type,8,1)),hq));
        rejected(() -> PlaybackSelection.choose(Arrays.asList(cap,new Codec(new CodecType(894,"aptX Adaptive"),8,2)),hq));
        rejected(() -> PlaybackSelection.parse(new String[]{"select","active","lossless","hq","96000","24"}));
        rejected(() -> PlaybackSelection.parse(new String[]{"select","active","lossless","ll","44100","16"}));
        rejected(() -> PlaybackSelection.parse(new String[]{"select","active","system"}));
        PlaybackSelection.Request ll=PlaybackSelection.parse(new String[]{"select","active","adaptive","ll","48000","24"});
        check(((Codec)PlaybackSelection.build(cap,ll)).mode==8192);
        CodecType losslessType=new CodecType(965,"aptX Lossless");
        Codec losslessCap=new Codec(losslessType,1,1);
        PlaybackSelection.Request lossless=PlaybackSelection.parse(new String[]{"select","active","lossless","hq","44100","16"});
        Codec losslessWanted=(Codec)PlaybackSelection.build(PlaybackSelection.choose(Arrays.asList(cap,losslessCap),lossless),lossless);
        check(losslessWanted.type==losslessType && losslessWanted.mode==0);
        check(PlaybackSelection.selectable(new Status(Arrays.asList(cap,losslessCap))).size()==2);
        check(PlaybackSelection.selectable(new Status(new Codec[]{cap})).get(0)==cap);
        check(PlaybackSelection.describe(losslessCap,true).get("modes").equals(Arrays.asList("hq")));
        check(PlaybackSelection.parse(new String[]{"get"}).family==null);
        check(PlaybackSelection.address("ab:cd:ef:01:23:45").equals("AB:CD:EF:01:23:45"));
        check(PlaybackSelection.address("active").equals("active"));
        rejected(() -> PlaybackSelection.address("AB:CD:EF:01:23"));
        rejected(() -> PlaybackSelection.address("AB:CD:EF:01:23:45;id"));
        rejected(() -> PlaybackSelection.parse(new String[]{"get","active","extra"}));
        rejected(() -> PlaybackSelection.parse(new String[]{"select","active","adaptive","hq","096000","24"}));
        rejected(() -> PlaybackSelection.parse(new String[]{"select","active","adaptive","unknown","48000","24"}));
        check(!PlaybackSelection.matches(null,wanted,hq));
        check(!PlaybackSelection.matches(new Codec(new CodecType(894,"aptX Adaptive"),8,2),wanted,hq));
        Codec multiRate=new Codec(type,10,2); multiRate.mode=4096;
        check(!PlaybackSelection.matches(multiRate,wanted,hq));
        Codec mono=new Codec(type,8,2); mono.channel=1;
        rejected(() -> PlaybackSelection.build(mono,hq));
        Codec duplicate=new Codec(new CodecType(731,"aptX Adaptive"),8,2);
        check(PlaybackSelection.choose(Arrays.asList(cap,duplicate),hq)==duplicate);
        Codec unknown=new Codec(new CodecType(421,"Vendor codec"),2,1);
        check(PlaybackSelection.describe(unknown,true).get("modes").equals(Collections.emptyList()));
        check(Boolean.FALSE.equals(PlaybackSelection.describe(null,false).get("available")));
        System.out.println("Playback selection: "+count+" checks passed (dynamic extended identities, exact peer combinations, ambiguity, modes, observation, list/array capabilities).");
    }
}
