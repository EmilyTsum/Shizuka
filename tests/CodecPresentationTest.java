// SPDX-License-Identifier: GPL-2.0-only
public final class CodecPresentationTest {
    static int assertions;
    static void check(boolean condition) {
        assertions++;
        if (!condition) throw new AssertionError();
    }
    public static void main(String[] args) {
        String profile = CodecPresentation.SAMSUNG_DZIF;
        check(CodecPresentation.losslessSelection(profile, false, 12));
        check(!CodecPresentation.losslessSelection(profile, true, 12));
        check(!CodecPresentation.losslessSelection("other", false, 12));
        check(!CodecPresentation.losslessSelection(profile, false, 7));
        check(CodecPresentation.name(profile, false, 12, "Vendor").equals("aptX Lossless (selection)"));
        check(CodecPresentation.name(profile, false, 13, "Vendor").equals("aptX LL"));
        check(CodecPresentation.name("other", false, 12, "Vendor codec").equals("Vendor codec"));
        check(CodecPresentation.name(profile, true, 13, "LC3").equals("LC3"));
        check(CodecPresentation.name("", false, 12, null).equals("Codec 12"));
        check(CodecPresentation.name("", false, 12, " ").equals("Codec 12"));
        check(CodecPresentation.mode(profile, 12, 0).equals("Lossless request"));
        check(CodecPresentation.mode(profile, 7, 4096).equals("HQ"));
        check(CodecPresentation.mode(profile, 7, 8192).equals("Low latency"));
        check(CodecPresentation.mode(profile, 7, 0).equals("Default"));
        check(CodecPresentation.mode(profile, 7, 123456) == null);
        check(CodecPresentation.mode(profile, 4, 4096) == null);
        check(CodecPresentation.mode("other", 7, 4096) == null);
        System.out.println("Codec presentation: " + assertions + " checks; system names retained and private mode words scoped to the backend.");
    }
}
