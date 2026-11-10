/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/init_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define check(v) do {++checks;if(!(v)){fprintf(stderr,"check %u failed at line %d\n",checks,__LINE__);abort();}} while(0)
/* Actual seventh INIT, whose normal APM basic ACK succeeded. Mode0x1197 is
 * HQ0x1000 plus capability0x01<<8 plus capability0x97, not a new user mode. */
static const uint8_t actual[59]={
    0x02,0,0,0,0xe7,0x03,0,0,0,0,0,0,0x14,0,0,0,0x64,0,0,0,
    0x14,0,0,0,0x64,0,0,0,0x14,0,0,0,0x64,0,0,0,0x97,0x11,0,0,
    0,0,0,0,0xff,0,0,0,0x01,0x97,0,0,0x0f,0x02,0x03,0x03,0x03,0,0xaa
};
static S25LatencyContext context;
static uint8_t packet[60];
static void put32(void *p,uint32_t v)
{uint8_t *d=p;for(unsigned i=0;i<4;i++)d[i]=(uint8_t)(v>>(i*8));}
static void init(void)
{
    context=(S25LatencyContext){S25_LATENCY_CONTEXT_MAGIC,1,64,10,1,44100,4096,2,0x1234,{0}};
    memcpy(packet,actual,59);packet[59]=0xee;
}
static int invoke(size_t size,size_t capacity,struct shizuka_init_profile_fields *diag)
{
    uint8_t before[60];S25LatencyContext c=context;memcpy(before,packet,sizeof packet);
    int result=shizuka_init_profile_hq_r3(packet,size,capacity,&context,diag);
    check(!memcmp(before,packet,sizeof packet));check(!memcmp(&c,&context,sizeof context));
    return result;
}
int main(void)
{
    struct shizuka_init_profile_fields diag;
    init();check(invoke(59,59,&diag));
    check(diag.raw_mode==0x1197 && diag.sample_rate_enum==2);
    check(diag.sink_capability_0==1 && diag.sink_capability_1==0x97 && !diag.reserved[0] && !diag.reserved[1]);
    check(invoke(59,60,NULL));
    uint8_t *exact=malloc(59);check(exact!=NULL);memcpy(exact,actual,59);
    check(shizuka_init_profile_hq_r3(exact,59,59,&context,&diag));free(exact);
    uint8_t unaligned[60];memcpy(unaligned+1,actual,59);
    check(shizuka_init_profile_hq_r3(unaligned+1,59,59,&context,NULL));
    for(size_t n=0;n<59;n++) {
        memset(&diag,0xa5,sizeof diag);struct shizuka_init_profile_fields before=diag;
        check(!invoke(59,n,&diag));check(!memcmp(&diag,&before,sizeof diag));
        check(!invoke(n,59,&diag));check(!memcmp(&diag,&before,sizeof diag));
    }
    check(!invoke(60,60,&diag));check(!invoke(SIZE_MAX,SIZE_MAX,&diag));
    check(!invoke(59,SIZE_MAX,&diag));
    check(!shizuka_init_profile_hq_r3(NULL,59,59,&context,&diag));
    check(!shizuka_init_profile_hq_r3((void *)(UINTPTR_MAX-7),59,59,&context,&diag));
    const uint32_t bad_modes[]={0,0x1000,0x2000,0x2197,0x3000,0x3197,0x4000,0x4197,
                                0x1097,0x1182,0x1196,0x1197|0x80000000,UINT32_MAX};
    for(unsigned i=0;i<sizeof bad_modes/sizeof(*bad_modes);i++) {
        init();put32(packet+36,bad_modes[i]);check(!invoke(59,59,&diag));
        check(diag.raw_mode==bad_modes[i]); /* rejected value remains diagnostic */
    }
    for(unsigned rate=0;rate<=4;rate++)if(rate!=2) {
        init();put32(packet,rate);check(!invoke(59,59,&diag));check(diag.sample_rate_enum==rate);
    }
    /* Exhaustively check capability1's reviewed R3 bits; additional capability
     * bits are allowed only when exactly mirrored by the real raw mode word. */
    for(unsigned cap=0;cap<=255;cap++) {
        init();packet[49]=(uint8_t)cap;put32(packet+36,0x1100|cap);
        check(invoke(59,59,&diag)==((cap&0x82)==0x82));
    }
    for(unsigned cap=0;cap<=255;cap++)if(cap!=1) {
        init();packet[48]=(uint8_t)cap;put32(packet+36,0x1000|(cap<<8)|0x97);
        check(!invoke(59,59,&diag));
    }
    for(unsigned field=0;field<16;field++) {
        init();
        switch(field) {
        case 0:context.magic++;break;
        case 1:context.abi++;break;
        case 2:context.size--;break;
        case 3:context.sequence=0;break;
        case 4:context.sequence=11;break;
        case 5:context.valid=0;break;
        case 6:context.valid=2;break;
        case 7:context.mode=8192;break;
        case 8:context.mode=12288;break;
        case 9:context.sample_rate=48000;break;
        case 10:context.sample_rate=96000;break;
        case 11:context.private_flags=1;break;
        case 12:context.private_flags=3;break;
        case 13:context.private_flags=0;break;
        case 14:context.reserved[0]=1;break;
        case 15:context.reserved[6]=1;break;
        }
        check(!invoke(59,59,&diag));
    }
    init();check(!shizuka_init_profile_hq_r3(packet,59,59,NULL,NULL));
    check(!shizuka_init_profile_hq_r3(packet,59,59,(void *)(UINTPTR_MAX-7),NULL));
    uint8_t bad_context[sizeof context+4];uintptr_t aligned=((uintptr_t)bad_context+3)&~(uintptr_t)3;
    check(!shizuka_init_profile_hq_r3(packet,59,59,(void *)(aligned+1),NULL));
    printf("init_profile: %u checks; failures=0\n",checks);
    return 0;
}
