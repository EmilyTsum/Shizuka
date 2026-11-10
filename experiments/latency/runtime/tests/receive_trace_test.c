/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/receive_trace.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define check(v) do {++checks;if(!(v)){fprintf(stderr,"check %u failed at line %d\n",checks,__LINE__);abort();}} while(0)
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void packet(uint8_t *p,size_t size,uint8_t value)
{memset(p,value,size);put32(p,((uint32_t)size<<8)|0x60u);}
static int capture(struct shizuka_receive_trace *t,const void *p,size_t n)
{
    /* A weak try-lock may fail spuriously. Test retries are outside the helper;
     * production callbacks never retry a skipped diagnostic record. */
    int result=0;
    for(unsigned i=0;i<1000 && !result;i++)result=shizuka_receive_trace_capture(t,p,n);
    return result;
}
static void stable(struct shizuka_receive_trace *t,uint8_t *out)
{check(shizuka_receive_trace_snapshot(t,out,56)==56);}
static void preserved(struct shizuka_receive_trace *t,const uint8_t *before)
{uint8_t out[56];stable(t,out);check(!memcmp(before,out,56));}

struct holder {struct shizuka_receive_trace *t;uint32_t ready,release;};
static void *hold_writer(void *argument)
{
    struct holder *h=argument;
    __atomic_store_n(&h->t->writer,1,__ATOMIC_RELEASE);
    __atomic_store_n(&h->ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&h->release,__ATOMIC_ACQUIRE))sched_yield();
    __atomic_store_n(&h->t->writer,0,__ATOMIC_RELEASE);
    return NULL;
}
struct stress {struct shizuka_receive_trace trace;uint32_t done,failed;};
static void *produce(void *argument)
{
    struct stress *s=argument;
    uint8_t a[56],b[56];packet(a,56,0x11);packet(b,56,0x22);
    for(unsigned i=0;i<200000;i++) {
        int result=shizuka_receive_trace_capture(&s->trace,(i&1)?a:b,56);
        if(result<0)__atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&s->done,1,__ATOMIC_RELEASE);
    return NULL;
}
static void *consume(void *argument)
{
    struct stress *s=argument;uint8_t out[56];
    do {
        memset(out,0xa5,56);
        size_t n=shizuka_receive_trace_snapshot(&s->trace,out,56);
        if(n) {
            if(n!=56 || out[0]!=0x60 || out[1]!=56 || out[2] || out[3]
                || (out[4]!=0x11 && out[4]!=0x22))
                __atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
            for(unsigned i=4;i<56;i++)if(out[i]!=out[4])
                __atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
        } else for(unsigned i=0;i<56;i++)if(out[i]!=0xa5)
            __atomic_store_n(&s->failed,1,__ATOMIC_RELAXED);
    } while(!__atomic_load_n(&s->done,__ATOMIC_ACQUIRE));
    return NULL;
}
int main(void)
{
    struct shizuka_receive_trace t={0};uint8_t out[56],before[56];
    memset(out,0xa5,56);memcpy(before,out,56);
    check(shizuka_receive_trace_snapshot(&t,out,56)==0);
    check(!memcmp(before,out,56));
    const size_t sizes[]={24,25,40,55,56,57,1024};
    for(unsigned k=0;k<sizeof(sizes)/sizeof(*sizes);k++) {
        size_t n=sizes[k];uint8_t *p=malloc(n),*saved=malloc(n);
        check(p && saved);packet(p,n,(uint8_t)(0x30+k));memcpy(saved,p,n);
        check(capture(&t,p,n)==1);stable(&t,out);
        check(!memcmp(out,saved,n<56?n:56));
        for(size_t i=n;i<56;i++)check(!out[i]);
        check(!memcmp(saved,p,n));
        memset(p,0xbb,n);free(p); /* snapshot owns bytes, not the packet */
        preserved(&t,out);free(saved);
    }
    /* A short final packet must not expose payload left by a longer one. */
    uint8_t p[1025];packet(p,24,0x77);check(capture(&t,p,24)==1);stable(&t,out);
    for(unsigned i=24;i<56;i++)check(!out[i]);
    memcpy(before,out,56);
    for(size_t n=0;n<24;n++)check(shizuka_receive_trace_capture(&t,p,n)==-1);
    check(shizuka_receive_trace_capture(&t,p,1025)==-1);
    check(shizuka_receive_trace_capture(&t,p,SIZE_MAX)==-1);
    check(shizuka_receive_trace_capture(NULL,p,24)==-1);
    check(shizuka_receive_trace_capture(&t,NULL,24)==-1);
    check(shizuka_receive_trace_capture(&t,(void *)(UINTPTR_MAX-8),24)==-1);
    const uint32_t malformed[]={0x60,0x1760,0x1960,0x1861,0x1850,0xffffff60};
    for(unsigned i=0;i<sizeof(malformed)/sizeof(*malformed);i++) {
        put32(p,malformed[i]);check(shizuka_receive_trace_capture(&t,p,24)==-1);
        preserved(&t,before);
    }
    uint8_t unaligned[sizeof(t)+4];
    uintptr_t aligned=((uintptr_t)unaligned+3u)&~(uintptr_t)3u;
    check(shizuka_receive_trace_capture((void *)(aligned+1),p,24)==-1);
    check(shizuka_receive_trace_capture(&t,&t,24)==-1);
    check(shizuka_receive_trace_capture(&t,(uint8_t *)&t+8,24)==-1);
    memset(out,0xa5,56);memcpy(before,out,56);
    check(shizuka_receive_trace_snapshot(NULL,out,56)==0);
    check(shizuka_receive_trace_snapshot(&t,NULL,56)==0);
    check(shizuka_receive_trace_snapshot(&t,out,55)==0);
    check(shizuka_receive_trace_snapshot(&t,&t,56)==0);
    check(shizuka_receive_trace_snapshot(&t,(void *)(UINTPTR_MAX-8),56)==0);
    check(!memcmp(before,out,56));
    __atomic_store_n(&t.sequence,3,__ATOMIC_RELEASE);
    check(shizuka_receive_trace_snapshot(&t,out,56)==0);
    check(!memcmp(before,out,56));
    __atomic_store_n(&t.sequence,UINT32_MAX-1,__ATOMIC_RELAXED);
    packet(p,56,0x33);check(capture(&t,p,56)==1);
    check(__atomic_load_n(&t.sequence,__ATOMIC_ACQUIRE)==2);
    stable(&t,before);
    struct holder h={.t=&t};pthread_t thread;
    check(!pthread_create(&thread,NULL,hold_writer,&h));
    while(!__atomic_load_n(&h.ready,__ATOMIC_ACQUIRE))sched_yield();
    packet(p,56,0x66);
    /* Exactly one attempt while another writer owns the try-lock. */
    check(shizuka_receive_trace_capture(&t,p,56)==0);
    check(__atomic_load_n(&t.sequence,__ATOMIC_ACQUIRE)==2);preserved(&t,before);
    __atomic_store_n(&h.release,1,__ATOMIC_RELEASE);check(!pthread_join(thread,NULL));
    check(capture(&t,p,56)==1);stable(&t,out);check(!memcmp(out,p,56));
    struct stress s={0};pthread_t writer,reader;
    check(!pthread_create(&reader,NULL,consume,&s));
    check(!pthread_create(&writer,NULL,produce,&s));
    check(!pthread_join(writer,NULL));check(!pthread_join(reader,NULL));
    check(!__atomic_load_n(&s.failed,__ATOMIC_RELAXED));
    printf("receive_trace: %u checks; 200000 concurrent writes; failures=0\n",checks);
    return 0;
}
