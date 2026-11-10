// SPDX-License-Identifier: GPL-2.0-only
import java.util.Objects;

public final class CodecSnapshotTest {
    private static int checks;
    private interface Task { void run() throws Exception; }
    private static void check(boolean value) { checks++; if (!value) throw new AssertionError("check " + checks); }
    private static void rejects(Task task) throws Exception {
        try { task.run(); throw new AssertionError("expected rejection"); }
        catch (IllegalArgumentException | IllegalStateException | NoSuchMethodException expected) { checks++; }
    }
    public static class Legacy {
        public static final int SAMPLE_RATE_44100=1,SAMPLE_RATE_96000=8,BITS_PER_SAMPLE_16=1,BITS_PER_SAMPLE_24=2;
        final int type, priority, rate, bits, channels;
        final long[] words;
        Legacy(Builder b) { type=b.type; priority=b.priority; rate=b.rate; bits=b.bits; channels=b.channels; words=b.words.clone(); }
        public int getCodecType() { return type; }
        public int getCodecPriority() { return priority; }
        public int getSampleRate() { return rate; }
        public int getBitsPerSample() { return bits; }
        public int getChannelMode() { return channels; }
        public long getCodecSpecific1() { return words[0]; }
        public long getCodecSpecific2() { return words[1]; }
        public long getCodecSpecific3() { return words[2]; }
        public long getCodecSpecific4() { return words[3]; }
        public static class Builder {
            int type=7, priority=17, rate=1, bits=1, channels=1;
            long[] words={4096, Long.MIN_VALUE, -1, Long.MAX_VALUE};
            public Builder setCodecType(int v) { type=v; return this; }
            public Builder setCodecPriority(int v) { priority=v; return this; }
            public Builder setSampleRate(int v) { rate=v; return this; }
            public Builder setBitsPerSample(int v) { bits=v; return this; }
            public Builder setChannelMode(int v) { channels=v; return this; }
            public Builder setCodecSpecific1(long v) { words[0]=v; return this; }
            public Builder setCodecSpecific2(long v) { words[1]=v; return this; }
            public Builder setCodecSpecific3(long v) { words[2]=v; return this; }
            public Builder setCodecSpecific4(long v) { words[3]=v; return this; }
            public Legacy build() { return new Legacy(this); }
        }
    }
    public static final class Identity {
        final long id;
        Identity(long id) { this.id=id; }
        @Override public boolean equals(Object value) { return value instanceof Identity && ((Identity)value).id==id; }
        @Override public int hashCode() { return Objects.hash(id); }
    }
    public static class Modern extends Legacy {
        final Identity identity;
        Modern(Builder b) { super(b); identity=b.identity; }
        public Identity getExtendedCodecType() { return identity; }
        public static class Builder extends Legacy.Builder {
            Identity identity;
            public Builder setExtendedCodecType(Identity value) { identity=value; return this; }
            @Override public Modern build() { return new Modern(this); }
        }
    }
    public static class OtherLegacy extends Legacy {
        OtherLegacy(Legacy.Builder b) { super(b); }
    }
    public static class DroppingBuilderCodec extends Modern {
        DroppingBuilderCodec(Builder b) { super(b); }
        public static class Builder extends Modern.Builder {
            public Builder() { words[2]=0; }
            @Override public Builder setCodecSpecific3(long ignored) { return this; }
            @Override public DroppingBuilderCodec build() { return new DroppingBuilderCodec(this); }
        }
    }
    public static class AmbiguousSetterCodec extends Modern {
        AmbiguousSetterCodec(Builder b) { super(b); }
        public static class Builder extends Modern.Builder {
            public Builder setExtendedCodecType(Object value) { identity=(Identity)value; return this; }
            @Override public AmbiguousSetterCodec build() { return new AmbiguousSetterCodec(this); }
        }
    }
    public static class MissingWord {
        public int getCodecType() { return 7; }
        public int getCodecPriority() { return 0; }
        public int getSampleRate() { return 1; }
        public int getBitsPerSample() { return 1; }
        public int getChannelMode() { return 2; }
        public long getCodecSpecific1() { return 0; }
        public long getCodecSpecific2() { return 0; }
        public long getCodecSpecific3() { return 0; }
    }
    public static void main(String[] args) throws Exception {
        Legacy original=new Legacy.Builder().build();
        CodecSnapshot saved=CodecSnapshot.capture(original);
        Legacy restored=(Legacy)saved.cloneObject();
        check(saved.matches(restored));
        check(restored.priority==17 && restored.channels==1); // Neither highest priority nor stereo is forced.
        for(int i=0;i<4;i++) check(restored.words[i]==original.words[i]);
        original.words[2]=34; // A later observable user/configuration change cannot be mistaken for the snapshot.
        check(!saved.matches(original));
        check(((Legacy)saved.cloneObject()).words[2]==-1);
        check(saved.advertisedBy(new Legacy.Builder().setSampleRate(3).setBitsPerSample(3).setChannelMode(3).build()));
        check(!saved.advertisedBy(new Legacy.Builder().setSampleRate(2).build()));
        check(!saved.advertisedBy(new Legacy.Builder().setCodecType(12).build()));
        check(!saved.matches(new OtherLegacy(new Legacy.Builder())));

        Identity vendor=new Identity(0x1122334455667788L);
        Modern observed=new Modern.Builder().setExtendedCodecType(vendor).build();
        CodecSnapshot modern=CodecSnapshot.capture(observed);
        Modern copy=(Modern)modern.cloneObject();
        check(copy.identity==vendor);
        check(modern.matches(copy));
        Modern.Builder advertised=new Modern.Builder().setExtendedCodecType(new Identity(vendor.id));
        advertised.setSampleRate(3);advertised.setBitsPerSample(3);advertised.setChannelMode(3);
        check(modern.advertisedBy(advertised.build()));
        check(!modern.advertisedBy(new Modern.Builder().setExtendedCodecType(new Identity(42)).build()));
        check(!modern.advertisedBy(new Legacy.Builder().build()));
        check(!saved.advertisedBy(copy));
        check(!modern.matches(new Modern.Builder().setExtendedCodecType(new Identity(42)).build()));
        check(!modern.matches(new Modern.Builder().setExtendedCodecType(vendor).setCodecSpecific2(9).build()));

        rejects(() -> CodecSnapshot.capture(new MissingWord()));
        rejects(() -> CodecSnapshot.capture(new Modern.Builder().build()));
        for(int value:new int[]{0,3,-1}) {
            rejects(() -> CodecSnapshot.capture(new Legacy.Builder().setSampleRate(value).build()));
            rejects(() -> CodecSnapshot.capture(new Legacy.Builder().setBitsPerSample(value).build()));
            rejects(() -> CodecSnapshot.capture(new Legacy.Builder().setChannelMode(value).build()));
        }
        DroppingBuilderCodec.Builder broken=new DroppingBuilderCodec.Builder();
        broken.setExtendedCodecType(vendor);broken.words[2]=71;
        rejects(() -> CodecSnapshot.capture(broken.build()).cloneObject());
        AmbiguousSetterCodec.Builder ambiguous=new AmbiguousSetterCodec.Builder();
        ambiguous.setExtendedCodecType(vendor);
        rejects(() -> CodecSnapshot.capture(ambiguous.build()).cloneObject());
        System.out.println("Exact codec snapshot: "+checks+" checks passed; no Bluetooth service calls.");
    }
}
