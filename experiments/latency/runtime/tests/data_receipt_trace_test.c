/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/data_receipt_trace.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define check(v) do {++checks;if(!(v)){fprintf(stderr,"check %u failed at line %d\n",checks,__LINE__);abort();}} while(0)
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t g32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t g64(const uint8_t *p){return g32(p)|(uint64_t)g32(p+4)<<32;}
static void make(uint8_t *p,size_t n,uint32_t opcode,uint32_t token)
{
    memset(p,0,n);p32(p,((uint32_t)n<<8)|0x60u);p[4]=3;p[5]=2;
    p32(p+8,opcode==0x05001005u?0x43f7u:0x43fau);p32(p+12,0x535a6101u);
    p32(p+16,token);p32(p+20,opcode);
}
static int capture(struct shizuka_data_receipt_trace *t,uint64_t e,const uint8_t *p,size_t n,uint64_t time)
{int rc=0;for(unsigned i=0;i<1000&&!rc;i++)rc=shizuka_data_receipt_trace_capture(t,e,p,n,time);return rc;}
static void snapshot(struct shizuka_data_receipt_trace *t,uint8_t *out)
{check(shizuka_data_receipt_trace_snapshot(t,out,512)==512);}
static void fresh(struct shizuka_data_receipt_trace *t){memset(t,0,sizeof(*t));}

struct stress {struct shizuka_data_receipt_trace trace;uint32_t done,failed;};
static void *produce(void *arg)
{
    struct stress *s=arg;uint8_t p[80];
    for(uint64_t epoch=1;epoch<=30000;epoch++) {
        make(p,80,0x05001005u,(uint32_t)epoch);
        p32(p+40,(uint32_t)epoch);p32(p+76,(uint32_t)epoch^0xa5a5a5a5u);
        shizuka_data_receipt_trace_capture(&s->trace,epoch,p,80,epoch);
    }
    __atomic_store_n(&s->done,1,__ATOMIC_RELEASE);return NULL;
}
static void *consume(void *arg)
{
    struct stress *s=arg;uint8_t out[512];
    do {
        memset(out,0xa5,sizeof(out));
        size_t n=shizuka_data_receipt_trace_snapshot(&s->trace,out,512);
        if(n) {
            uint32_t epoch=(uint32_t)g64(out+24);const uint8_t *r=out+32;
            if(n!=512 || memcmp(out,"SHZDTR1\0",8) || g32(out+8)!=1 ||
                g32(out+12)!=512 || !g32(out+16) || (g32(out+16)&1) || g32(out+20)!=1 ||
                g32(r+8)!=epoch || g32(r+32)!=epoch || g32(r+36)!=(epoch^0xa5a5a5a5u) ||
                g64(r)!=((uint64_t)epoch*1000000u) || g32(r+56))
                __atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
            for(unsigned i=92;i<512;i++)if(out[i])__atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
        }else for(unsigned i=0;i<512;i++)if(out[i]!=0xa5)__atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
    }while(!__atomic_load_n(&s->done,__ATOMIC_ACQUIRE));
    return NULL;
}

int main(void)
{
    struct shizuka_data_receipt_trace t={0};uint8_t out[512],saved[512],p[1025];
    memset(out,0xa5,512);memcpy(saved,out,512);
    check(!shizuka_data_receipt_trace_snapshot(&t,out,512));check(!memcmp(out,saved,512));
    make(p,80,0x05001005u,0xa8c30001);p32(p+24,7);p32(p+60,9);
    p32(p+28,0xf1f2f3f4);p32(p+32,0xe1e2e3e4);p32(p+36,0xd1d2d3d4);
    p32(p+64,0xc1c2c3c4);p32(p+68,0xb1b2b3b4);p32(p+72,0x91929394);
    p32(p+40,664);p32(p+76,68);p32(p+52,0x80000000);p32(p+56,1);
    p32(p+44,0x11223344);p32(p+48,0x55667788);
    check(capture(&t,9,p,80,1234)==1);snapshot(&t,out);
    check(!memcmp(out,"SHZDTR1\0",8));check(g32(out+8)==1&&g32(out+12)==512);
    check(g32(out+16)==2&&g32(out+20)==1&&g64(out+24)==9);
    const uint8_t *r=out+32;
    check(g64(r)==1234000000);check(g32(r+8)==0xa8c30001);
    check(g32(r+12)==0x43f7&&g32(r+16)==0x535a6101&&g32(r+20)==0x05001005);
    check(g32(r+24)==7&&g32(r+28)==9&&g32(r+32)==664&&g32(r+36)==68);
    check(g32(r+40)==0x80000000&&g32(r+44)==1&&g64(r+48)==UINT64_C(0x5566778811223344));
    check(!g32(r+56));
    /* Pointers/addresses/handles in the payload must not appear in the record. */
    const uint32_t omitted[]={0xf1f2f3f4,0xe1e2e3e4,0xd1d2d3d4,0xc1c2c3c4,0xb1b2b3b4,0x91929394};
    for(unsigned i=0;i<60-3;i++)for(unsigned k=0;k<6;k++)check(g32(r+i)!=omitted[k]);
    make(p,56,0x05001004u,0xa8c30002);p32(p+36,3);p32(p+52,5);
    check(capture(&t,9,p,56,1235)==1);snapshot(&t,out);r=out+92;
    check(g32(out+20)==2&&g32(r+8)==0xa8c30002&&g32(r+24)==3&&g32(r+28)==5);
    for(unsigned i=32;i<56;i++)check(!r[i]);
    make(p,32,0x02001005u,0xa8c30003);p32(p+24,0x0400100a);p32(p+28,19);
    check(capture(&t,9,p,32,1236)==1);snapshot(&t,out);r=out+152;
    check(g32(r+24)==19&&g32(r+40)==0x0400100a&&g32(r+8)==0xa8c30003);
    /* Positive basic/unrelated events cannot fill the first-eight receipt window. */
    p32(p+28,0);check(!shizuka_data_receipt_trace_capture(&t,9,p,32,0));
    make(p,24,0x03001004u,0);check(!shizuka_data_receipt_trace_capture(&t,9,p,24,0));
    for(unsigned i=3;i<8;i++){
        make(p,80,0x05001005u,i);check(capture(&t,9,p,80,i)==1);
    }
    snapshot(&t,saved);check(g32(saved+20)==8);
    make(p,80,0x05001005u,88);check(!shizuka_data_receipt_trace_capture(&t,9,p,80,0));
    check(!shizuka_data_receipt_trace_capture(&t,8,p,80,0));snapshot(&t,out);check(!memcmp(out,saved,512));
    /* A new epoch discards all old receipts, including stale unused bytes. */
    check(capture(&t,10,p,80,4)==1);snapshot(&t,out);check(g32(out+20)==1&&g64(out+24)==10);
    for(unsigned i=92;i<512;i++)check(!out[i]);
    /* Truncated exact allocations exercise every possible endpoint without an OOB read. */
    for(unsigned n=24;n<80;n++){
        uint8_t *small=malloc(n);check(small!=NULL);make(small,n,0x05001005u,n);
        p32(small,0x5060);check(capture(&t,11+n,small,n,0)==1);snapshot(&t,out);
        check(g32(out+32+56)&SHIZUKA_DATA_RECEIPT_BAD_LENGTH);
        for(unsigned i=24;i<56;i++)check(!out[32+i]);free(small);
    }
    const uint32_t headers[]={0x5061,0x5050,0x60,0xffffff60};
    for(unsigned i=0;i<4;i++){
        make(p,80,0x05001005u,0);p32(p,headers[i]);check(capture(&t,100+i,p,80,0)==1);
        snapshot(&t,out);check(g32(out+88)&(SHIZUKA_DATA_RECEIPT_BAD_HEADER|SHIZUKA_DATA_RECEIPT_BAD_LENGTH));
    }
    make(p,80,0x05001005u,0);p[4]=2;p[5]=3;p[7]=1;p32(p+8,0x43fa);p32(p+12,99);
    check(capture(&t,110,p,80,UINT64_MAX)==1);snapshot(&t,out);
    check(g32(out+88)==(SHIZUKA_DATA_RECEIPT_BAD_DOMAIN|SHIZUKA_DATA_RECEIPT_BAD_PORT|
        SHIZUKA_DATA_RECEIPT_BAD_RESERVED|SHIZUKA_DATA_RECEIPT_TIME_OVERFLOW));
    check(g64(out+32)==UINT64_MAX);
    make(p,24,0x02001005u,0);check(capture(&t,111,p,24,0)==1);snapshot(&t,out);
    check(g32(out+88)==SHIZUKA_DATA_RECEIPT_BAD_LENGTH);
    /* Invalid caller ranges and aliasing leave state/output unchanged. */
    snapshot(&t,saved);
    for(unsigned n=0;n<24;n++)check(shizuka_data_receipt_trace_capture(&t,112,p,n,0)==-1);
    check(shizuka_data_receipt_trace_capture(&t,112,p,1025,0)==-1);
    check(shizuka_data_receipt_trace_capture(&t,112,p,SIZE_MAX,0)==-1);
    check(shizuka_data_receipt_trace_capture(&t,0,p,24,0)==-1);
    check(shizuka_data_receipt_trace_capture(NULL,112,p,24,0)==-1);
    check(shizuka_data_receipt_trace_capture(&t,112,NULL,24,0)==-1);
    check(shizuka_data_receipt_trace_capture(&t,112,&t,80,0)==-1);
    check(shizuka_data_receipt_trace_capture(&t,112,(void *)(UINTPTR_MAX-8),80,0)==-1);
    check(!shizuka_data_receipt_trace_snapshot(&t,&t,512));
    check(!shizuka_data_receipt_trace_snapshot(&t,out,511));
    check(!shizuka_data_receipt_trace_snapshot(&t,NULL,512));
    check(!shizuka_data_receipt_trace_snapshot(&t,(void *)(UINTPTR_MAX-8),512));
    snapshot(&t,out);check(!memcmp(out,saved,512));
    /* One callback attempt must skip immediately while any other writer owns it. */
    __atomic_store_n(&t.writer,1,__ATOMIC_RELEASE);make(p,80,0x05001005u,1);
    check(!shizuka_data_receipt_trace_capture(&t,112,p,80,0));snapshot(&t,out);check(!memcmp(out,saved,512));
    __atomic_store_n(&t.writer,0,__ATOMIC_RELEASE);
    __atomic_store_n(&t.wire.sequence,UINT32_MAX-1,__ATOMIC_RELEASE);
    check(capture(&t,112,p,80,0)==1);snapshot(&t,out);check(g32(out+16)==2);
    __atomic_store_n(&t.wire.sequence,3,__ATOMIC_RELEASE);memset(out,0xa5,512);
    check(!shizuka_data_receipt_trace_snapshot(&t,out,512));for(unsigned i=0;i<512;i++)check(out[i]==0xa5);
    fresh(&t);struct stress s={0};pthread_t writer,reader;
    check(!pthread_create(&reader,NULL,consume,&s));check(!pthread_create(&writer,NULL,produce,&s));
    check(!pthread_join(writer,NULL));check(!pthread_join(reader,NULL));
    check(!__atomic_load_n(&s.failed,__ATOMIC_RELAXED));
    printf("data_receipt_trace: %u checks; 30000 concurrent epochs; failures=0\n",checks);
    return 0;
}
