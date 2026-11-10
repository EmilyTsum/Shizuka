/* SPDX-License-Identifier: GPL-2.0-only
 * Synthetic-only RAW-island graph/transaction/calibration bounds tests. */
#include "relay_calibration.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !SHIZUKA_RELAY_RAW_ISLAND
#error "This test exercises the explicitly enabled RAW-island experiment"
#endif

#define SG SHIZUKA_RELAY_SG
#define E5 SHIZUKA_RELAY_ENCODER_CONTAINER
#define E6 SHIZUKA_RELAY_CONTAINER
#define RD SHIZUKA_RELAY_RD
#define WR SHIZUKA_RELAY_WR
#define CLIENT UINT32_C(0x535a6101)
struct blob { uint8_t d[8192];size_t n; };
static unsigned checks;
#define CHECK(x) do {++checks;assert(x);} while(0)
static void word(struct blob *b,uint32_t v)
{ assert(b->n+4<=sizeof b->d);for(unsigned i=0;i<4;i++)b->d[b->n++]=(uint8_t)(v>>(8*i)); }
static void set(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static size_t begin(struct blob *b,uint32_t mid,uint32_t pid)
{ size_t p=b->n;word(b,mid);word(b,pid);word(b,0);word(b,0);return p; }
static void end(struct blob *b,size_t p)
{ set(b->d+p+8,(uint32_t)(b->n-p-16));while(b->n&7)b->d[b->n++]=0xa5; }
static void sg(struct blob *b,uint32_t id)
{
    size_t p=begin(b,1,0x08001001);word(b,1);word(b,id);word(b,2);
    word(b,0x0800100f);word(b,4);word(b,2);word(b,0x08001010);word(b,4);word(b,1);end(b,p);
}
static void property(struct blob *b,uint32_t pid,uint32_t value)
{ word(b,pid);word(b,4);word(b,value); }
static void container(struct blob *b,uint32_t id,int expanded)
{
    word(b,id);word(b,7);property(b,0x08001014,2);property(b,0x08001013,4096);
    word(b,0x08001011);word(b,8);word(b,1);word(b,0x0b001001);
    property(b,0x08001012,UINT32_MAX);property(b,0x080010cb,expanded?CLIENT:UINT32_MAX);
    property(b,0x08001174,1);property(b,0x0800124d,1);
}
static void pair(struct blob *b,uint32_t mid,uint32_t iid) {word(b,mid);word(b,iid);}
static void group(struct blob *b,uint32_t id,int expanded)
{
    word(b,SG);word(b,id);word(b,id==E5?(expanded?7:4):(expanded?1:4));
    if(id==E5) {
        pair(b,0x07001010,0x43f3);pair(b,0x0700101a,0x43f4);
        pair(b,0x07001011,0x43f5);pair(b,0x07001041,0x43f6);
        if(expanded) {pair(b,0x07001001,RD);pair(b,0x07001003,0x43f8);pair(b,0x07001082,0x43fb);}
    } else if(expanded)pair(b,0x07001000,WR);
    else {pair(b,0x0700101a,RD);pair(b,0x07001003,0x43f8);pair(b,0x0700101a,WR);pair(b,0x07001082,0x43fb);}
}
static void edge(struct blob *b,uint32_t a,uint32_t port,uint32_t c)
{ word(b,a);word(b,port);word(b,c);word(b,2); }
static struct blob fixture(int expanded,int reverse)
{
    struct blob b={{0},0};size_t p;
    sg(&b,0xb000006c);p=begin(&b,1,0x08001004);word(&b,1);edge(&b,WR,1,0x4098);end(&b,p);
    sg(&b,SG);p=begin(&b,1,0x08001000);word(&b,2);
    container(&b,reverse?E6:E5,expanded);container(&b,reverse?E5:E6,expanded);end(&b,p);
    p=begin(&b,1,0x08001002);word(&b,2);group(&b,reverse?E6:E5,expanded);group(&b,reverse?E5:E6,expanded);end(&b,p);
    static const uint32_t ids[]={0x43f3,0x43f4,0x43f5,0x43f6,RD,0x43f8,WR,0x43fb};
    p=begin(&b,1,0x08001003);word(&b,8);
    for(unsigned i=0;i<8;i++) {
        uint32_t id=ids[i];word(&b,id);word(&b,1);word(&b,0x08001015);word(&b,8);
        word(&b,id==0x43f3?5:(id==WR&&expanded?0:1));
        word(&b,id==0x43f5?3:(id==RD&&expanded?0:1));
    }end(&b,p);
    p=begin(&b,1,0x08001004);word(&b,expanded?7:8);
    edge(&b,0x43f3,1,0x43f4);edge(&b,0x43f4,1,0x43f5);edge(&b,0x43f5,1,0x43f6);
    edge(&b,0x43f6,1,0x43f8);edge(&b,0x43f5,3,0x44e9);
    edge(&b,0x43f8,1,expanded?0x43fb:RD);edge(&b,0x43fb,1,expanded?RD:WR);
    if(!expanded)edge(&b,RD,1,0x43fb);end(&b,p);
    p=begin(&b,1,0x08001061);word(&b,1);word(&b,0x4edd);word(&b,0xc0000001);
    word(&b,0x43fb);word(&b,0xc0000001);word(&b,2);
    word(&b,0x08001062);word(&b,8);word(&b,1);word(&b,0x080010d7);
    word(&b,0x0800136f);word(&b,4);word(&b,1);end(&b,p);
    p=begin(&b,0x43fb,0x08001997);for(unsigned i=0;i<59;i++)b.d[b.n++]=(uint8_t)i;end(&b,p);
    if(expanded) {
        p=begin(&b,RD,0x0800114d);word(&b,1);word(&b,CLIENT);end(&b,p);
        p=begin(&b,WR,0x080010c0);word(&b,1);word(&b,CLIENT);end(&b,p);
        p=begin(&b,RD,0x08001007);word(&b,1);word(&b,1);end(&b,p);
    }
    sg(&b,0xb0000099);p=begin(&b,0x4099,0x08001996);word(&b,0xdeadbeef);end(&b,p);
    return b;
}

static void bounds_and_oracle(void)
{
    for(int reverse=0;reverse<2;reverse++) {
        struct blob in=fixture(0,reverse),expected=fixture(1,reverse);size_t n=123;
        uint8_t *out=malloc(expected.n+16);CHECK(out);memset(out,0x7b,expected.n+16);
        CHECK(shizuka_relay_graph_measure(in.d,in.n,CLIENT,&n)==0&&n==expected.n);
        CHECK(shizuka_relay_graph_transform(in.d,in.n,CLIENT,out,expected.n,&n)==0&&n==expected.n);
        CHECK(memcmp(out,expected.d,n)==0);CHECK(shizuka_relay_graph_verify(out,n,CLIENT)==0);
        for(unsigned i=0;i<16;i++)CHECK(out[n+i]==0x7b);
        struct blob saved=in;
        CHECK(shizuka_relay_graph_transform(in.d,in.n,CLIENT,in.d,sizeof in.d,&n)==SHIZUKA_GRAPH_OVERLAP);
        CHECK(n==0&&memcmp(&saved,&in,sizeof in)==0);
        memset(out,0x7b,expected.n);CHECK(shizuka_relay_graph_transform(in.d,in.n,CLIENT,out,expected.n-1,&n)==SHIZUKA_GRAPH_CAPACITY);
        CHECK(n==0);for(size_t i=0;i<expected.n;i++)CHECK(out[i]==0x7b);
        for(size_t size=0;size<in.n;size++) {
            uint8_t *exact=malloc(size?size:1);CHECK(exact);memcpy(exact,in.d,size);
            int rc=shizuka_relay_graph_transform(exact,size,CLIENT,out,expected.n,&n);
            if(!rc)CHECK(n==size+56&&shizuka_relay_graph_verify(out,n,CLIENT)==0);
            else CHECK(n==0);
            CHECK(memcmp(exact,in.d,size)==0);free(exact);
        }
        for(size_t i=0;i<in.n;i++)for(unsigned bit=0;bit<8;bit++) {
            uint8_t *exact=malloc(in.n);CHECK(exact);memcpy(exact,in.d,in.n);exact[i]^=(uint8_t)(1u<<bit);
            int rc=shizuka_relay_graph_transform(exact,in.n,CLIENT,out,expected.n,&n);
            if(!rc)CHECK(n==expected.n&&shizuka_relay_graph_verify(out,n,CLIENT)==0);
            else CHECK(n==0);
            free(exact);
        }
        free(out);
    }
}

static void transaction_and_calibration(void)
{
    struct blob in=fixture(0,0),expected=fixture(1,0);uint8_t input[8192],workspace[8192];
    struct shizuka_relay_scope scope={0x10000,0x20000,7,0x2012,CLIENT,15,1,0,0};
    struct shizuka_relay_provider provider={10,1,150000};
    uint32_t ids[]={0xb000006c,SG,0xb0000099};
    struct shizuka_gsl_key keys[]={{0xa2000000,0xa2000003},{0xb4000000,0xb4000002},{0xb5000000,0xb5000003}};
    struct shizuka_gsl_after_call call={0x246e4,0xacdb0002,24,28,0,(uint32_t)in.n,ids,3,keys,3,0,0};
    struct shizuka_relay_transaction t={0};struct shizuka_relay_after_result_data result;
    CHECK(shizuka_relay_after_acdb(&t,&scope,&provider,&call,workspace,sizeof workspace,&result)==SHIZUKA_RELAY_ADVERTISE);
    CHECK(result.advertised_size==expected.n);provider.enabled=0;provider.revision=11;
    call.caller_rva=0x247d0;
    CHECK(shizuka_relay_after_acdb(&t,&scope,&provider,&call,workspace,sizeof workspace,&result)==SHIZUKA_RELAY_ADVERTISE);
    memcpy(input,in.d,in.n);call.caller_rva=0x24e20;call.spf=input;call.spf_capacity=sizeof input;
    CHECK(shizuka_relay_after_acdb(&t,&scope,&provider,&call,workspace,sizeof workspace,&result)==SHIZUKA_RELAY_COMMITTED);
    CHECK(memcmp(input,expected.d,expected.n)==0&&t.provider.revision==10);
    CHECK(shizuka_relay_open_complete(&t,&scope,1)==1);
    struct blob cal={{0},0},kept={{0},0};size_t p;
    p=begin(&cal,RD,0x08001026);word(&cal,1);end(&cal,p);
    p=begin(&kept,0x43f6,0x080010d0);word(&kept,44100);end(&kept,p); /* RAT */
    p=begin(&kept,0x43f8,0x08001024);word(&kept,32);end(&kept,p); /* converter */
    p=begin(&kept,0x43fb,0x08001183);for(unsigned i=0;i<59;i++)kept.d[kept.n++]=(uint8_t)i;end(&kept,p);
    memcpy(cal.d+cal.n,kept.d,kept.n);cal.n+=kept.n;
    p=begin(&cal,WR,0x08001031);word(&cal,2);end(&cal,p);
    struct shizuka_relay_cal_transaction ct={0};struct shizuka_relay_cal_result cr;
    struct shizuka_gsl_cal_call cc={0x1e1c0,0xacdb0004,36,12,0,ids,3,keys,3,0,0,0,0,0,(uint32_t)cal.n,0};
    CHECK(shizuka_relay_after_calibration(&t,&ct,&scope,&cc,&cr)==SHIZUKA_RELAY_BYPASS);
    cc.caller_rva=0x1e2d8;cc.data=cal.d;cc.capacity=sizeof cal.d;
    CHECK(shizuka_relay_after_calibration(&t,&ct,&scope,&cc,&cr)==SHIZUKA_RELAY_COMMITTED);
    CHECK(cr.removed==2&&cr.actual_size==kept.n&&memcmp(cal.d,kept.d,kept.n)==0);
    shizuka_relay_close(&t,&scope);CHECK(t.phase==SHIZUKA_RELAY_CLOSED);
}
int main(void)
{ bounds_and_oracle();transaction_and_calibration();printf("RAW island: %u checks passed\n",checks);return 0; }
