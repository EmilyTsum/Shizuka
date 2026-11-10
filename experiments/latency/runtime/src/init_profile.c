/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/init_profile.h"
static uint32_t get32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int valid_range(const void *p,size_t size,size_t alignment)
{return p && !((uintptr_t)p&(alignment-1)) && size<=UINTPTR_MAX-(uintptr_t)p;}
static int context_hq(const S25LatencyContext *c)
{
    if(!valid_range(c,sizeof(*c),4) || c->magic!=S25_LATENCY_CONTEXT_MAGIC
        || c->abi!=S25_LATENCY_CONTEXT_ABI || c->size!=sizeof(*c)
        || !c->sequence || (c->sequence&1) || c->valid!=1 || c->mode!=4096
        || c->sample_rate!=44100 || c->private_flags!=2)return 0;
    for(unsigned i=0;i<7;i++)if(c->reserved[i])return 0;
    return 1;
}
int shizuka_init_profile_hq_r3(const void *payload,size_t size,size_t capacity,
    const S25LatencyContext *context,struct shizuka_init_profile_fields *diagnostics)
{
    if(size!=SHIZUKA_ADAPTIVE_INIT_BYTES || capacity<size || !valid_range(payload,capacity,1)
        || (diagnostics && !valid_range(diagnostics,sizeof(*diagnostics),4)))return 0;
    const uint8_t *p=payload;
    uint32_t raw=get32(p+36),rate=get32(p);
    uint8_t cap0=p[48],cap1=p[49];
    if(diagnostics) {
        diagnostics->raw_mode=raw;diagnostics->sample_rate_enum=rate;
        diagnostics->sink_capability_0=cap0;diagnostics->sink_capability_1=cap1;
        diagnostics->reserved[0]=diagnostics->reserved[1]=0;
    }
    return rate==2 && cap0==1 && (cap1&0x82)==0x82
        && raw==(UINT32_C(0x1000)|((uint32_t)cap0<<8)|cap1) && context_hq(context);
}
