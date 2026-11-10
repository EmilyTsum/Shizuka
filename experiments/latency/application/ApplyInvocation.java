// SPDX-License-Identifier: GPL-2.0-only
/** Parse every public/internal field before selecting an Android service path. */
final class ApplyInvocation {
    enum Kind { PREPARE, STATUS, WORKER }
    final Kind kind;final String id,guardianStart;final int hq,ll,guardian;
    private final String[] arguments;
    private ApplyInvocation(Kind k,String id,int hq,int ll,int guardian,String ticks,String[] args){kind=k;this.id=id;this.hq=hq;this.ll=ll;this.guardian=guardian;guardianStart=ticks;arguments=args.clone();}
    String[] arguments(){return arguments.clone();}
    static ApplyInvocation parse(String[] args) {
        if(args==null||args.length==0)throw new IllegalArgumentException("Missing application command");
        if(args[0].equals("status")&&(args.length==1||args.length==2&&uuid(args[1])))return new ApplyInvocation(Kind.STATUS,args.length==2?args[1]:null,0,0,0,null,args);
        if(args[0].equals("prepare")&&args.length==4&&uuid(args[1]))return new ApplyInvocation(Kind.PREPARE,args[1],(int)ApplicationInputs.decimal(args[2],150),(int)ApplicationInputs.decimal(args[3],100),0,null,args);
        if(args[0].equals("worker")&&args.length==4&&uuid(args[1])) {
            int pid=(int)ApplicationInputs.decimal(args[2],Integer.MAX_VALUE);long ticks=ApplicationInputs.decimal(args[3],Long.MAX_VALUE);
            if(pid>1&&ticks>0)return new ApplyInvocation(Kind.WORKER,args[1],0,0,pid,args[3],args);
        }
        throw new IllegalArgumentException("Fixed status, prepare or worker arguments required");
    }
    private static boolean uuid(String s){return s!=null&&s.matches(ApplicationInputs.UUID);}
}
