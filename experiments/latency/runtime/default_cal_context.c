/* SPDX-License-Identifier: GPL-2.0-only */
#include "default_cal_context.h"

_Static_assert(sizeof(uintptr_t)==8,"stock GSL ABI is ARM64");
static uint32_t get32(const void *p)
{
    const uint8_t *b=p;
    return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
}
static uintptr_t get_pointer(const void *p)
{return get32(p)|(uintptr_t)get32((const uint8_t *)p+4)<<32;}
static int range_valid(uintptr_t p,size_t size,size_t alignment)
{return p && !(p&(alignment-1)) && size<=UINTPTR_MAX-p;}
static int same_owner(const struct shizuka_relay_scope *a,const struct shizuka_relay_scope *b)
{
    return a->graph && a->thread && a->epoch && a->source_port
        && a->source_port!=UINT32_MAX && a->client_port
        && a->source_port!=a->client_port
        && a->graph==b->graph && a->thread==b->thread && a->epoch==b->epoch
        && a->source_port==b->source_port && a->client_port==b->client_port;
}
static int fresh(const struct shizuka_relay_scope *s)
{
    const uint32_t ownership=SHIZUKA_GSL_EXACT_LIBRARY|SHIZUKA_GSL_NAMESPACE_OWNED
        |SHIZUKA_GSL_SOURCE_REGISTERED;
    return (s->flags&ownership)==ownership && s->open_refs==1
        && !s->start_refs && !s->stop_refs;
}
static int same_sg_set(const struct shizuka_relay_transaction *owner,const uint8_t *cmd)
{
    uint32_t count=get32(cmd);
    uintptr_t values=get_pointer(cmd+4);
    if(!count || count>SHIZUKA_GSL_MAX_SGIDS || count!=owner->num_sgids
        || !range_valid(values,(size_t)count*4,4))return 0;
    uint32_t target=0;
    for(uint32_t i=0;i<count;i++) {
        uint32_t value=get32((const uint8_t *)values+(size_t)i*4),matches=0;
        if(!value)return 0;
        for(uint32_t j=0;j<i;j++)
            if(value==get32((const uint8_t *)values+(size_t)j*4))return 0;
        for(uint32_t j=0;j<count;j++)if(owner->sgids[j]==value)++matches;
        if(matches!=1)return 0;
        target+=value==SHIZUKA_RELAY_SG;
    }
    return target==1;
}
int shizuka_default_cal_context_view(const struct shizuka_graph_bridge *bridge,
    const struct shizuka_graph_bridge_env *env,uint32_t pc,
    const void *command,uint32_t command_size,const void *response,uint32_t response_size,
    uintptr_t node,uintptr_t fp,uintptr_t (*strip_ip)(uintptr_t),
    struct shizuka_default_cal_view *output)
{
    if(!bridge || !env || !strip_ip || !output || (pc!=0x1e1c0 && pc!=0x1e2d8)
        || command_size!=36 || response_size!=12 || !bridge->open_accepted
        || bridge->transaction.phase!=SHIZUKA_RELAY_ACTIVE
        || bridge->transaction.profile_confirmed || !bridge->frozen_context_seq
        || (bridge->frozen_context_seq&1) || !env->library_base
        || env->library_base!=bridge->library_base || env->library_base>UINTPTR_MAX-0x251f0
        || !same_owner(&bridge->transaction.scope,&env->scope)
        || !fresh(&bridge->transaction.scope) || !fresh(&env->scope))return 0;
    /* Bind the gateway's original x29 to these actual stock stack objects.
     * CAL and its helper both allocate0x190 and set FP=SP+0x130. */
    if(fp<0x50 || !range_valid(fp,0x1a0,16)
        || (uintptr_t)command!=fp-0x40 || (uintptr_t)response!=fp-0x50)return 0;
    uintptr_t parent=get_pointer((const void *)fp);
    if(parent!=fp+0x190
        || strip_ip(get_pointer((const void *)(fp+8)))!=env->library_base+0x251f0
        || strip_ip(get_pointer((const void *)(parent+8)))!=env->library_base+0x23e80)return 0;
    if(!range_valid(node,0x28,8)
        || get_pointer((const void *)(parent-0xe0))!=node
        || get32((const void *)(node+0x18)) || get_pointer((const void *)(node+0x20)))return 0;
    if(!same_sg_set(&bridge->transaction,command))return 0;
    uintptr_t vector=get_pointer((const void *)(parent-0xd8));
    if(!range_valid(vector,16,8))return 0;
    uint32_t count=get32((const void *)vector);
    uintptr_t values=get_pointer((const void *)(vector+8));
    if(!count || count>SHIZUKA_GSL_MAX_KEYS || count!=bridge->transaction.num_keys
        || !range_valid(values,(size_t)count*8,4))return 0;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *pair=(const uint8_t *)values+(size_t)i*8;
        uint32_t key=get32(pair),value=get32(pair+4);
        if(key!=bridge->transaction.keys[i].key || value!=bridge->transaction.keys[i].value)return 0;
        for(uint32_t j=0;j<i;j++)
            if(key==get32((const uint8_t *)values+(size_t)j*8))return 0;
    }
    output->keys=(const struct shizuka_gsl_key *)values;output->num_keys=count;
    return 1;
}
