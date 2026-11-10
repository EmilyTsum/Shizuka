/* SPDX-License-Identifier: GPL-2.0-only */
#include "relay_calibration.h"

static uint32_t get(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int same_owner(const struct shizuka_relay_scope *a,const struct shizuka_relay_scope *b)
{
    return a->graph==b->graph && a->epoch==b->epoch && a->source_port==b->source_port
        && a->client_port==b->client_port;
}
static int vector_valid(const struct shizuka_gsl_key *p,uint32_t n)
{
    if(n>SHIZUKA_GSL_MAX_KEYS || (n && !p))return 0;
    for(uint32_t i=0;i<n;i++)for(uint32_t j=0;j<i;j++)if(p[i].key==p[j].key)return 0;
    return 1;
}
static int same_vector(const struct shizuka_gsl_key *a,const struct shizuka_gsl_key *b,uint32_t n)
{ for(uint32_t i=0;i<n;i++)if(a[i].key!=b[i].key || a[i].value!=b[i].value)return 0;return 1; }
static int call_valid(const struct shizuka_relay_transaction *owner,const struct shizuka_gsl_cal_call *c)
{
    if(c->opcode!=0xacdb0004 || c->command_size!=36 || c->response_size!=12
        || !c->sgids || !c->num_sgids || c->num_sgids>SHIZUKA_GSL_MAX_SGIDS
        || !vector_valid(c->current_gkv,c->num_gkv) || c->num_gkv!=owner->num_keys
        || !same_vector(owner->keys,c->current_gkv,c->num_gkv)
        || !vector_valid(c->prior_ckv,c->num_prior_ckv) || !vector_valid(c->new_ckv,c->num_new_ckv))return 0;
    unsigned target=0;
    for(uint32_t i=0;i<c->num_sgids;i++) {
        if(c->sgids[i]==SHIZUKA_RELAY_SG)++target;
        for(uint32_t j=0;j<i;j++)if(c->sgids[i]==c->sgids[j])return 0;
    }
    return target==1;
}
static int record(const uint8_t *data,size_t size,size_t pos,size_t *total,int *remove)
{
    if(pos>size || size-pos<16)return 0;
    uint32_t iid=get(data+pos),pid=get(data+pos+4),n=get(data+pos+8);
    if(n>SHIZUKA_RELAY_MAX_SPF || get(data+pos+12))return 0;
    size_t padded=((size_t)n+7)&~(size_t)7;
    if(padded>size-pos-16)return 0;
    *total=16+padded;
    *remove=(iid==SHIZUKA_RELAY_RD || iid==SHIZUKA_RELAY_WR)
        && (pid==0x08001026 || pid==0x08001031 || pid==0x08001313);
    return 1;
}
static int reject(struct shizuka_relay_cal_transaction *t)
{ t->pending=0;return SHIZUKA_RELAY_ABORT; }

void shizuka_relay_calibration_reset(struct shizuka_relay_cal_transaction *t)
{ if(t) { uint8_t *p=(uint8_t *)t;for(size_t i=0;i<sizeof(*t);i++)p[i]=0; } }

int shizuka_relay_after_calibration(const struct shizuka_relay_transaction *owner,
    struct shizuka_relay_cal_transaction *t,const struct shizuka_relay_scope *scope,
    const struct shizuka_gsl_cal_call *c,struct shizuka_relay_cal_result *result)
{
    if(!owner || !t || !scope || !c || !result)return SHIZUKA_RELAY_ABORT;
    result->actual_size=c->data_size;result->removed=0;
    if(c->caller_rva!=0x1e1c0 && c->caller_rva!=0x1e2d8)return SHIZUKA_RELAY_BYPASS;
    if(owner->phase!=SHIZUKA_RELAY_ACTIVE || !same_owner(&owner->scope,scope))return SHIZUKA_RELAY_BYPASS;
    /* One GSL graph can calibrate disjoint SG subsets. No reference to the
     * replaced SG means this is not our calibration transaction. In particular
     * it must not clear another thread's pending target query. */
    if(c->sgids&&c->num_sgids&&c->num_sgids<=SHIZUKA_GSL_MAX_SGIDS){
        uint32_t target=0;for(uint32_t i=0;i<c->num_sgids;++i)target+=c->sgids[i]==SHIZUKA_RELAY_SG;
        if(!target)return SHIZUKA_RELAY_BYPASS;
    }
    if(c->original_status) { t->pending=0;return SHIZUKA_RELAY_BYPASS; }
    if(!call_valid(owner,c))return reject(t);
    if(c->caller_rva==0x1e1c0) {
        if(t->pending || c->data || c->data_size>SHIZUKA_RELAY_MAX_SPF || !scope->thread)return reject(t);
        t->scope=*scope;t->pending=1;t->capacity=c->data_size;
        t->num_sgids=c->num_sgids;t->num_prior_ckv=c->num_prior_ckv;t->num_new_ckv=c->num_new_ckv;
        for(uint32_t i=0;i<c->num_sgids;i++)t->sgids[i]=c->sgids[i];
        for(uint32_t i=0;i<c->num_prior_ckv;i++)t->prior_ckv[i]=c->prior_ckv[i];
        for(uint32_t i=0;i<c->num_new_ckv;i++)t->new_ckv[i]=c->new_ckv[i];
        return SHIZUKA_RELAY_BYPASS; /* old size/capacity deliberately unchanged */
    }
    if(!t->pending || !same_owner(&t->scope,scope) || t->scope.thread!=scope->thread
        || t->num_sgids!=c->num_sgids || t->num_prior_ckv!=c->num_prior_ckv || t->num_new_ckv!=c->num_new_ckv
        || !same_vector(t->prior_ckv,c->prior_ckv,c->num_prior_ckv)
        || !same_vector(t->new_ckv,c->new_ckv,c->num_new_ckv)
        || c->data_size>t->capacity || c->capacity<t->capacity || (c->data_size && !c->data))return reject(t);
    for(uint32_t i=0;i<t->num_sgids;i++)if(t->sgids[i]!=c->sgids[i])return reject(t);
    size_t pos=0,kept=0;uint32_t count=0,removed=0;
    while(pos<c->data_size) {
        size_t total;int strip;
        if(!record(c->data,c->data_size,pos,&total,&strip) || ++count>4096)return reject(t);
        if(strip)++removed;else kept+=total;
        pos+=total;
    }
    /* All checks complete before the first in-place edit. Source always lies
     * at/above destination, so forward copying safely compacts overlaps.
     */
    pos=0;size_t write=0;
    while(pos<c->data_size) {
        size_t total;int strip;
        if(!record(c->data,c->data_size,pos,&total,&strip))return reject(t);
        if(!strip) { for(size_t i=0;i<total;i++)c->data[write+i]=c->data[pos+i];write+=total; }
        pos+=total;
    }
    t->pending=0;result->actual_size=(uint32_t)kept;result->removed=removed;
    return kept?SHIZUKA_RELAY_COMMITTED:SHIZUKA_RELAY_CAL_EMPTY;
}
