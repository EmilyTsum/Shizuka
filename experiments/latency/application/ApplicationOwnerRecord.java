// SPDX-License-Identifier: GPL-2.0-only
import java.nio.charset.StandardCharsets;

/** Immutable receipt decoded from the lifecycle owner's private fixed record. */
final class ApplicationOwnerRecord {
    final String id,boot,cookie,token,config,guardianStart,oldStart,newStart,phase,captureHash,packageHash,activationHash;
    final int hq,ll,guardian,oldHal,newHal;final long graph;
    ApplicationOwnerRecord(String text) {
        if(text==null||text.length()>1024||!text.endsWith("\n")||text.indexOf('\n')!=text.length()-1)throw new IllegalArgumentException("Invalid owner receipt size/lines");
        String[] f=text.substring(0,text.length()-1).split(" ",-1);if(f.length!=19||!f[0].equals("1"))throw new IllegalArgumentException("Unknown owner receipt ABI");
        id=f[1];boot=f[2];cookie=f[3];token=f[4];config=f[5];hq=(int)ApplicationInputs.decimal(f[6],150);ll=(int)ApplicationInputs.decimal(f[7],100);
        guardian=(int)ApplicationInputs.decimal(f[8],Integer.MAX_VALUE);guardianStart=f[9];oldHal=(int)ApplicationInputs.decimal(f[10],Integer.MAX_VALUE);oldStart=f[11];
        newHal=(int)ApplicationInputs.decimal(f[12],Integer.MAX_VALUE);newStart=f[13];graph=ApplicationInputs.decimal(f[14],Long.MAX_VALUE);
        phase=f[15];captureHash=f[16];packageHash=f[17];activationHash=f[18];
        if(!id.matches(ApplicationInputs.UUID)||!boot.matches(ApplicationInputs.UUID)||!cookie.matches("[a-f0-9]{32}")||cookie.equals("00000000000000000000000000000000")||
            !token.matches(ApplicationInputs.HEX)||!config.matches(ApplicationInputs.HEX)||guardian<=1||oldHal<=1||
            ApplicationInputs.decimal(guardianStart,Long.MAX_VALUE)==0||ApplicationInputs.decimal(oldStart,Long.MAX_VALUE)==0||
            !phase.matches("armed|ready|waiting|applied|disabled|recovered|unconfirmed")||!captureHash.matches(ApplicationInputs.HEX)||!packageHash.matches(ApplicationInputs.HEX)||!activationHash.matches(ApplicationInputs.HEX))
            throw new IllegalArgumentException("Invalid owner receipt binding");
        long newTicks=ApplicationInputs.decimal(newStart,Long.MAX_VALUE);
        if(newHal==0?newTicks!=0||graph!=0:newHal<=1||newTicks==0)throw new IllegalArgumentException("Invalid new process pair");
        if(!phase.equals("armed")&&!phase.equals("unconfirmed")&&(newHal==0||newHal==oldHal&&newStart.equals(oldStart)))throw new IllegalArgumentException("Receipt has no new HAL identity");
        if(phase.equals("armed")&&(newHal!=0||!newStart.equals("0")||graph!=0))throw new IllegalArgumentException("ARM cannot claim a new session");
        if(phase.equals("applied")&&(graph==0||hq==0))throw new IllegalArgumentException("Applied receipt needs positive HQ and graph");
        if(phase.equals("disabled")&&(hq!=0||graph!=0))throw new IllegalArgumentException("Disabled receipt cannot carry a relay graph");
    }
    static ApplicationOwnerRecord decode(byte[] bytes)throws Exception{return new ApplicationOwnerRecord(ApplicationInputs.utf8(bytes));}
    byte[] bytes(){return ("1 "+id+" "+boot+" "+cookie+" "+token+" "+config+" "+hq+" "+ll+" "+guardian+" "+guardianStart+" "+oldHal+" "+oldStart+" "+newHal+" "+newStart+" "+graph+" "+phase+" "+captureHash+" "+packageHash+" "+activationHash+"\n").getBytes(StandardCharsets.US_ASCII);}
}
