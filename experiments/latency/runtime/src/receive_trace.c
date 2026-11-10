/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/receive_trace.h"

static uint32_t get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int range_valid(const void *p,size_t n)
{ return p && (uintptr_t)p<=UINTPTR_MAX-n; }
static int overlaps(const void *a,size_t an,const void *b,size_t bn)
{ return (uintptr_t)a<(uintptr_t)b+bn && (uintptr_t)b<(uintptr_t)a+an; }
int shizuka_receive_trace_capture(struct shizuka_receive_trace *t,const void *packet,size_t capacity)
{
    const uint8_t *p=packet;
    if(!range_valid(t,sizeof(*t)) || ((uintptr_t)t&3u) || capacity<24
        || capacity>SHIZUKA_RECEIVE_TRACE_MAX_PACKET || !range_valid(packet,capacity)
        || overlaps(t,sizeof(*t),packet,capacity))return -1;
    uint32_t header=get32(p),bytes=header>>8;
    if((header&255u)!=0x60 || bytes<24 || bytes>capacity)return -1;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&t->writer,&expected,1u,1,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
    uint32_t next=(__atomic_load_n(&t->sequence,__ATOMIC_RELAXED)+2u)&~1u;
    if(!next)next=2;
    __atomic_store_n(&t->sequence,next-1u,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for(uint32_t i=0;i<SHIZUKA_RECEIVE_TRACE_BYTES;i++)
        __atomic_store_n(t->bytes+i,i<bytes?p[i]:0,__ATOMIC_RELAXED);
    __atomic_store_n(&t->sequence,next,__ATOMIC_RELEASE);
    __atomic_store_n(&t->writer,0u,__ATOMIC_RELEASE);
    return 1;
}
size_t shizuka_receive_trace_snapshot(const struct shizuka_receive_trace *t,void *output,size_t capacity)
{
    if(!range_valid(t,sizeof(*t)) || ((uintptr_t)t&3u) || capacity<SHIZUKA_RECEIVE_TRACE_BYTES
        || !range_valid(output,SHIZUKA_RECEIVE_TRACE_BYTES)
        || overlaps(t,sizeof(*t),output,SHIZUKA_RECEIVE_TRACE_BYTES))return 0;
    uint32_t sequence=__atomic_load_n(&t->sequence,__ATOMIC_ACQUIRE);
    if(!sequence || (sequence&1u))return 0;
    uint8_t copy[SHIZUKA_RECEIVE_TRACE_BYTES];
    for(uint32_t i=0;i<SHIZUKA_RECEIVE_TRACE_BYTES;i++)copy[i]=__atomic_load_n(t->bytes+i,__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(sequence!=__atomic_load_n(&t->sequence,__ATOMIC_ACQUIRE))return 0;
    uint8_t *out=output;
    for(uint32_t i=0;i<SHIZUKA_RECEIVE_TRACE_BYTES;i++)out[i]=copy[i];
    return SHIZUKA_RECEIVE_TRACE_BYTES;
}
