/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/data_receipt_trace.h"

static uint32_t get32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p)
{return get32(p)|(uint64_t)get32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t *p,uint64_t v)
{put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static int valid(const void *p,size_t n)
{return p && (uintptr_t)p<=UINTPTR_MAX-n;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{return (uintptr_t)a<(uintptr_t)b+bn && (uintptr_t)b<(uintptr_t)a+an;}
static void store_bytes(uint8_t *p,const uint8_t *s,size_t n)
{for(size_t i=0;i<n;i++)__atomic_store_n(p+i,s?s[i]:0,__ATOMIC_RELAXED);}

int shizuka_data_receipt_trace_capture(struct shizuka_data_receipt_trace *t,
    uint64_t epoch,const void *packet,size_t capacity,uint64_t time_ms)
{
    if(!valid(t,sizeof(*t)) || ((uintptr_t)t&3u) || !epoch || capacity<24 ||
        capacity>SHIZUKA_DATA_RECEIPT_TRACE_MAX_PACKET || !valid(packet,capacity) ||
        overlap(t,sizeof(*t),packet,capacity))return -1;
    const uint8_t *p=packet;
    uint32_t opcode=get32(p+20),expected_size;
    if(opcode==0x05001005u)expected_size=80;
    else if(opcode==0x05001004u)expected_size=56;
    else if(opcode==0x02001005u)expected_size=32;
    else return 0;
    uint32_t header=get32(p),declared=header>>8,shape=0;
    if((header&255u)!=0x60u)shape|=SHIZUKA_DATA_RECEIPT_BAD_HEADER;
    if(declared!=expected_size || declared>capacity)shape|=SHIZUKA_DATA_RECEIPT_BAD_LENGTH;
    if(p[4]!=3 || p[5]!=2)shape|=SHIZUKA_DATA_RECEIPT_BAD_DOMAIN;
    uint32_t src=get32(p+8),dst=get32(p+12);
    if(dst!=0x535a6101u || (opcode==0x05001005u?src!=0x43f7u:
        opcode==0x05001004u?src!=0x43fau:src!=1u && src!=0x43f7u && src!=0x43fau))
        shape|=SHIZUKA_DATA_RECEIPT_BAD_PORT;
    if(p[7])shape|=SHIZUKA_DATA_RECEIPT_BAD_RESERVED;
    /* A well-formed successful BASIC cannot displace a useful data receipt.
     * For malformed BASIC, only read status if its bytes are accessible. */
    if(opcode==0x02001005u && !shape && !get32(p+28))return 0;
    uint8_t record[60]={0};
    uint64_t ns;
    if(time_ms>UINT64_MAX/1000000u){ns=UINT64_MAX;shape|=SHIZUKA_DATA_RECEIPT_TIME_OVERFLOW;}
    else ns=time_ms*1000000u;
    put64(record,ns);put32(record+8,get32(p+16));put32(record+12,src);
    put32(record+16,dst);put32(record+20,opcode);
    if(declared==expected_size && declared<=capacity){
        const uint8_t *b=p+24;
        if(opcode==0x05001005u){
            put32(record+24,get32(b));put32(record+28,get32(b+36));
            put32(record+32,get32(b+16));put32(record+36,get32(b+52));
            put32(record+40,get32(b+28));put32(record+44,get32(b+32));
            put64(record+48,get64(b+20));
        }else if(opcode==0x05001004u){
            put32(record+24,get32(b+12));put32(record+28,get32(b+28));
        }else{put32(record+24,get32(b+4));put32(record+40,get32(b));}
    }
    put32(record+56,shape);
    uint32_t unlocked=0;
    if(!__atomic_compare_exchange_n(&t->writer,&unlocked,1u,1,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
    uint64_t old_epoch=get64(t->bytes+24);
    uint32_t count=get32(t->bytes+20);
    if(epoch<old_epoch || (epoch==old_epoch && count>=8)){
        __atomic_store_n(&t->writer,0u,__ATOMIC_RELEASE);return 0;
    }
    uint32_t next=(__atomic_load_n(&t->wire.sequence,__ATOMIC_RELAXED)+2u)&~1u;
    if(!next)next=2;
    __atomic_store_n(&t->wire.sequence,next-1u,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if(epoch!=old_epoch){
        uint8_t head[32]={0};
        const uint8_t magic[8]={'S','H','Z','D','T','R','1',0};
        for(unsigned i=0;i<8;i++)head[i]=magic[i];
        put32(head+8,1);put32(head+12,512);put64(head+24,epoch);
        store_bytes(t->bytes,head,16);store_bytes(t->bytes+20,head+20,12);
        store_bytes(t->bytes+32,0,480);count=0;
    }
    store_bytes(t->bytes+32+count*60,record,60);
    uint8_t count_bytes[4];put32(count_bytes,count+1);
    store_bytes(t->bytes+20,count_bytes,4);
    __atomic_store_n(&t->wire.sequence,next,__ATOMIC_RELEASE);
    __atomic_store_n(&t->writer,0u,__ATOMIC_RELEASE);
    return 1;
}

size_t shizuka_data_receipt_trace_snapshot(const struct shizuka_data_receipt_trace *t,
    void *output,size_t capacity)
{
    if(!valid(t,sizeof(*t)) || ((uintptr_t)t&3u) || capacity<512 ||
        !valid(output,512) || overlap(t,sizeof(*t),output,512))return 0;
    uint32_t sequence=__atomic_load_n(&t->wire.sequence,__ATOMIC_ACQUIRE);
    if(!sequence || (sequence&1u))return 0;
    uint8_t copy[512];
    for(unsigned i=0;i<512;i++)if(i<16 || i>=20)
        copy[i]=__atomic_load_n(t->bytes+i,__ATOMIC_RELAXED);
    put32(copy+16,sequence);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(sequence!=__atomic_load_n(&t->wire.sequence,__ATOMIC_ACQUIRE))return 0;
    uint8_t *out=output;for(unsigned i=0;i<512;i++)out[i]=copy[i];
    return 512;
}
