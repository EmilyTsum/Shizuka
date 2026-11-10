/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/control_scope.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static uint8_t packet[70000],saved[70000],unaligned[70001];
static uint8_t owned_body[65536],body_before[65536];
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
#define check(value) do {++checks;if(!(value)){fprintf(stderr,"check %u failed at line %d\n",checks,__LINE__);abort();}} while(0)
static void header(uint32_t opcode)
{
    memset(packet,0,sizeof(packet));packet[4]=2;packet[5]=3;
    put32(packet+8,0x8001);put32(packet+12,1);put32(packet+20,opcode);
}
static void finish(size_t bytes)
{put32(packet,((uint32_t)bytes<<8)|0x60);put32(packet+36,(uint32_t)bytes-40);}
static size_t list(size_t at,uint32_t mid,const uint32_t *ids,uint32_t n)
{
    put32(packet+at,mid);put32(packet+at+4,0x08001005);put32(packet+at+8,4+4*n);
    put32(packet+at+16,n);
    for(uint32_t i=0;i<n;i++)put32(packet+at+20+4*i,ids[i]);
    return at+16+((4+4*(size_t)n+7)&~(size_t)7);
}
static size_t opaque(size_t at,uint32_t pid)
{
    put32(packet+at,1);put32(packet+at+4,pid);put32(packet+at+8,5);
    memcpy(packet+at+16,"bytes",5);memset(packet+at+21,0xa5,3);return at+24;
}
static void invalid_mutation(size_t bytes,size_t at,uint32_t value)
{
    memcpy(saved,packet,bytes);put32(packet+at,value);
    check(shizuka_control_scope_target(packet,bytes)==-1);memcpy(packet,saved,bytes);
}
int main(void)
{
    const uint32_t ids[]={0xb000006c,SHIZUKA_CONTROL_TARGET_SG,0xb0000099};
    const uint32_t absent[]={0xb000006c,0xb0000099};
    size_t size;
    for(uint32_t op=0x01001001;op<=0x01001004;op++) {
        header(op);size=list(40,1,ids,3);finish(size);
        check(shizuka_control_scope_target(packet,size)==1);
        check(shizuka_control_scope_target(packet,sizeof(packet))==1); /* extra allocation capacity */
        memcpy(unaligned+1,packet,size);check(shizuka_control_scope_target(unaligned+1,size)==1);
        header(op);size=list(40,1,absent,2);finish(size);
        check(shizuka_control_scope_target(packet,size)==0);
        header(op);size=opaque(40,0x08001004);finish(size);
        check(shizuka_control_scope_target(packet,size)==0); /* connection-only close */
        header(op);size=list(opaque(40,0x08001004),1,ids,3);size=opaque(size,0x08001061);finish(size);
        check(shizuka_control_scope_target(packet,size)==1);
        for(size_t n=0;n<size;n++)check(shizuka_control_scope_target(packet,n)==-1);
    }
    header(0x01001004);size=list(40,1,ids,3);finish(size);
    invalid_mutation(size,0,((uint32_t)size<<8)|0x50); /* wrong header words */
    invalid_mutation(size,0,((uint32_t)size<<8)|0x61); /* unknown GPR version */
    invalid_mutation(size,0,((uint32_t)(size+1)<<8)|0x60);
    invalid_mutation(size,0,0x60);
    invalid_mutation(size,4,0x0303); /* wrong DSP domain */
    invalid_mutation(size,4,0x0202); /* wrong host domain */
    invalid_mutation(size,4,0x01000302); /* reserved header byte */
    invalid_mutation(size,8,0);invalid_mutation(size,12,6);
    invalid_mutation(size,20,0x01001000);invalid_mutation(size,20,0x01001006);
    invalid_mutation(size,24,0x1000);invalid_mutation(size,28,1);invalid_mutation(size,32,123);
    invalid_mutation(size,36,(uint32_t)size-41);invalid_mutation(size,36,(uint32_t)size-39);
    invalid_mutation(size,40,0x43f7); /* same PID on non-APM module is ambiguous */
    invalid_mutation(size,48,UINT32_MAX);invalid_mutation(size,48,15);
    invalid_mutation(size,52,1);invalid_mutation(size,56,UINT32_MAX);
    invalid_mutation(size,56,2);invalid_mutation(size,60,0);
    invalid_mutation(size,68,ids[0]); /* duplicate unrelated SG */
    invalid_mutation(size,68,SHIZUKA_CONTROL_TARGET_SG);
    check(shizuka_control_scope_target(NULL,size)==-1);
    check(shizuka_control_scope_target(packet,SIZE_MAX)==-1);
    header(0x01001004);size=list(list(40,1,ids,3),1,absent,2);finish(size);
    check(shizuka_control_scope_target(packet,size)==-1); /* duplicate list parameter */
    header(0x01001004);size=list(40,1,NULL,0);finish(size);
    check(shizuka_control_scope_target(packet,size)==0);
    header(0x01001004);finish(40);check(shizuka_control_scope_target(packet,40)==0);
    /* A valid target followed by malformed data must not publish target proof. */
    header(0x01001004);size=list(40,1,ids,3);finish(size+8);
    check(shizuka_control_scope_target(packet,size+8)==-1);
    header(0x01001004);size=opaque(list(40,1,ids,3),0x08001004);finish(size);
    invalid_mutation(size,size-16,UINT32_MAX);
    /* Upper-bound unique list, target near the end, and late duplicate. */
    uint32_t large[SHIZUKA_CONTROL_MAX_SUBGRAPHS+1];
    for(unsigned i=0;i<SHIZUKA_CONTROL_MAX_SUBGRAPHS+1;i++)large[i]=0xb1000000+i;
    large[SHIZUKA_CONTROL_MAX_SUBGRAPHS-1]=SHIZUKA_CONTROL_TARGET_SG;
    header(0x01001004);size=list(40,1,large,SHIZUKA_CONTROL_MAX_SUBGRAPHS);finish(size);
    check(shizuka_control_scope_target(packet,size)==1);
    invalid_mutation(size,60+4*(SHIZUKA_CONTROL_MAX_SUBGRAPHS-1),large[0]);
    header(0x01001004);size=list(40,1,large,SHIZUKA_CONTROL_MAX_SUBGRAPHS+1);finish(size);
    check(shizuka_control_scope_target(packet,size)==-1);
    /* Packet cap is independent of the count/available allocation. */
    header(0x01001004);size=list(40,1,ids,3);
    while(size<=SHIZUKA_CONTROL_MAX_PACKET_BYTES)size=opaque(size,0x08001999);
    finish(size);check(size>SHIZUKA_CONTROL_MAX_PACKET_BYTES);
    check(shizuka_control_scope_target(packet,size)==-1);
    /* Deterministic mutation corpus: always bounded; non-error means a known
     * well-formed inband command, never an OOB pointer dereference. */
    header(0x01001004);size=list(40,1,ids,3);finish(size);memcpy(saved,packet,size);
    uint32_t rng=0x7c16094a;
    for(unsigned i=0;i<5000;i++) {
        memcpy(packet,saved,size);rng=rng*1664525+1013904223;
        size_t at=rng%size;rng=rng*1664525+1013904223;packet[at]^=(uint8_t)(1+(rng%255));
        int result=shizuka_control_scope_target(packet,size);check(result>=-1&&result<=1);
    }
    for(uint32_t op=0x01001001;op<=0x01001004;op++) {
        header(op);size=opaque(list(40,1,ids,3),0x08001004);finish(size);
        size_t body_size=size-40;
        memcpy(owned_body,packet+40,body_size);memcpy(body_before,owned_body,body_size);
        put32(packet,(40u<<8)|0x60);put32(packet+32,0x1234);
        check(shizuka_control_scope_target(packet,40)==-1); /* unknown OOB never dereferenced */
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,body_size)==1);
        check(!memcmp(owned_body,body_before,body_size));
        /* A zero address with a valid map handle is an offset, not NULL CPU memory. */
        check(!packet[24]&&!packet[28]);
        put32(packet+24,0xdeadbeef);put32(packet+28,0xfeefbead);
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,sizeof owned_body)==1);
        for(size_t n=0;n<body_size;n++)
            check(shizuka_control_scope_target_owned_body(packet,40,owned_body,n)==-1);
        check(shizuka_control_scope_target_owned_body(packet,39,owned_body,body_size)==-1);
        check(shizuka_control_scope_target_owned_body(packet,40,NULL,body_size)==-1);
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,65537)==-1);
        check(shizuka_control_scope_target_owned_body(packet,40,packet,body_size)==-1);
        check(shizuka_control_scope_target_owned_body(packet,64,packet+40,body_size)==-1);
        put32(packet+36,(uint32_t)body_size+1);
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,body_size)==-1);
        put32(packet+36,(uint32_t)body_size-1);
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,body_size)==-1);
        put32(packet+36,(uint32_t)body_size);put32(packet+32,0);
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,body_size)==-1);
        put32(packet+32,0x1234);put32(packet,(48u<<8)|0x60);
        check(shizuka_control_scope_target_owned_body(packet,48,owned_body,body_size)==-1);
        header(op);size=list(40,1,absent,2);finish(size);body_size=size-40;
        memcpy(owned_body,packet+40,body_size);put32(packet,(40u<<8)|0x60);put32(packet+32,0x1234);
        check(shizuka_control_scope_target_owned_body(packet,40,owned_body,body_size)==0);
    }
    /* Passive normal-START peer SG observation is deliberately distinct from
     * target00ab authority and cannot infer another command's state or ACK. */
    const uint32_t peer_absent[]={SHIZUKA_CONTROL_TARGET_SG,0xb0000099};
    header(0x01001002);size=list(40,1,ids,3);finish(size);
    check(shizuka_control_scope_start_peer_present(packet,size)==1);
    memcpy(unaligned+1,packet,size);check(shizuka_control_scope_start_peer_present(unaligned+1,size)==1);
    for(size_t n=0;n<size;n++)check(shizuka_control_scope_start_peer_present(packet,n)==-1);
    put32(packet+68,ids[0]);check(shizuka_control_scope_start_peer_present(packet,size)==-1);
    header(0x01001002);size=list(40,1,peer_absent,2);finish(size);
    check(shizuka_control_scope_start_peer_present(packet,size)==0&&shizuka_control_scope_target(packet,size)==1);
    header(0x01001002);size=list(40,1,absent,2);finish(size);
    check(shizuka_control_scope_start_peer_present(packet,size)==1&&shizuka_control_scope_target(packet,size)==0);
    header(0x01001002);size=opaque(40,0x08001004);finish(size);
    check(shizuka_control_scope_start_peer_present(packet,size)==0);
    header(0x01001002);size=list(40,1,ids,3);finish(size+8);
    check(shizuka_control_scope_start_peer_present(packet,size+8)==-1);
    header(0x01001002);size=list(40,1,ids,3);finish(size);
    put32(packet,(40u<<8)|0x60);put32(packet+32,123);
    check(shizuka_control_scope_start_peer_present(packet,40)==-1);
    for(uint32_t op=0x01001000;op<=0x01001006;op++)if(op!=0x01001002) {
        header(op);size=list(40,1,ids,3);finish(size);
        check(shizuka_control_scope_start_peer_present(packet,size)==-1);
    }
    check(shizuka_control_scope_start_peer_present(NULL,64)==-1);
    printf("control scope: %u checks; exact management ABI, explicit target, partial close, unknown/owned OOB, alias/capacity, duplicate/truncated lists, 4096 SG bound and 5000 mutations passed\n",checks);
}
