/* SPDX-License-Identifier: GPL-2.0-only */
#include "relay_transaction.h"

static int same_owner(const struct shizuka_relay_scope *a,const struct shizuka_relay_scope *b)
{
    return a->graph==b->graph && a->epoch==b->epoch
        && a->source_port==b->source_port && a->client_port==b->client_port;
}
static int same_scope(const struct shizuka_relay_scope *a,const struct shizuka_relay_scope *b)
{ return same_owner(a,b) && a->thread==b->thread; }
static int scope_ready(const struct shizuka_relay_scope *s)
{
    return s && s->graph && s->thread && s->epoch && s->source_port && s->source_port!=UINT32_MAX
        && s->source_port!=s->client_port
        && s->client_port>=0x4001 && s->client_port!=UINT32_MAX
        && s->client_port!=SHIZUKA_RELAY_RD && s->client_port!=SHIZUKA_RELAY_WR
        && (s->flags&SHIZUKA_GSL_REQUIRED_FLAGS)==SHIZUKA_GSL_REQUIRED_FLAGS
        && s->open_refs==1 && !s->start_refs && !s->stop_refs;
}
static int request_ready(const struct shizuka_gsl_after_call *c)
{
    if(!c || c->opcode!=0xacdb0002 || c->command_size!=24 || c->response_size!=28
        || !c->sgids || !c->num_sgids || c->num_sgids>SHIZUKA_GSL_MAX_SGIDS
        || !c->keys || !c->num_keys || c->num_keys>SHIZUKA_GSL_MAX_KEYS)return 0;
    unsigned target=0,device=0,profile=0,format=0;
    for(uint32_t i=0;i<c->num_sgids;i++) {
        if(c->sgids[i]==SHIZUKA_RELAY_SG)++target;
        for(uint32_t j=0;j<i;j++)if(c->sgids[i]==c->sgids[j])return 0;
    }
    for(uint32_t i=0;i<c->num_keys;i++) {
        uint32_t key=c->keys[i].key,value=c->keys[i].value;
        for(uint32_t j=0;j<i;j++)if(key==c->keys[j].key)return 0;
        if(key==0xa3000000)return 0; /* TX/voice */
        if(key==0xa2000000) { if(value!=0xa2000003)return 0;device=1; }
        if(key==0xb4000000) { if(value!=0xb4000002)return 0;profile=1; }
        if(key==0xb5000000) { if(value!=0xb5000003)return 0;format=1; }
    }
    return target==1 && device && profile && format;
}
static int same_request(const struct shizuka_relay_transaction *t,const struct shizuka_gsl_after_call *c)
{
    if(t->num_sgids!=c->num_sgids || t->num_keys!=c->num_keys)return 0;
    for(uint32_t i=0;i<t->num_sgids;i++)if(t->sgids[i]!=c->sgids[i])return 0;
    for(uint32_t i=0;i<t->num_keys;i++)
        if(t->keys[i].key!=c->keys[i].key || t->keys[i].value!=c->keys[i].value)return 0;
    return 1;
}
static int abort_open(struct shizuka_relay_transaction *t)
{ t->phase=SHIZUKA_RELAY_FAILED;t->profile_confirmed=0;return SHIZUKA_RELAY_ABORT; }

int shizuka_relay_after_acdb(struct shizuka_relay_transaction *t,
    const struct shizuka_relay_scope *s,const struct shizuka_relay_provider *provider,
    const struct shizuka_gsl_after_call *c,uint8_t *workspace,size_t capacity,
    struct shizuka_relay_after_result_data *out)
{
    if(!t || !c || !out)return SHIZUKA_RELAY_ABORT;
    out->advertised_size=c->original_spf_size;out->frozen_revision=0;
    uint32_t stage=0;
    if(c->caller_rva==0x246e4)stage=1;
    if(c->caller_rva==0x247d0)stage=2;
    if(c->caller_rva==0x24e20)stage=3;
    if(!stage)return SHIZUKA_RELAY_BYPASS;
    if(t->phase==SHIZUKA_RELAY_IDLE) {
        if(stage!=1 || c->original_status || !scope_ready(s) || !request_ready(c)
            || !provider || provider->enabled!=1 || !provider->revision
            || !provider->hq_extra_ttp_us || provider->hq_extra_ttp_us>150000
            || provider->hq_extra_ttp_us%1000 || !c->original_spf_size
            || c->original_spf_size>SHIZUKA_RELAY_MAX_SPF-SHIZUKA_RELAY_GRAPH_GROWTH)
            return SHIZUKA_RELAY_BYPASS;
        t->scope=*s;t->provider=*provider;t->phase=SHIZUKA_RELAY_SIZE;
        t->original_size=c->original_spf_size;t->profile_confirmed=0;
        t->num_sgids=c->num_sgids;t->num_keys=c->num_keys;
        for(uint32_t i=0;i<c->num_sgids;i++)t->sgids[i]=c->sgids[i];
        for(uint32_t i=0;i<c->num_keys;i++)t->keys[i]=c->keys[i];
        out->advertised_size=t->original_size+SHIZUKA_RELAY_GRAPH_GROWTH;
        out->frozen_revision=t->provider.revision;return SHIZUKA_RELAY_ADVERTISE;
    }
    if(!scope_ready(s) || !same_scope(&t->scope,s) || !request_ready(c)
        || !same_request(t,c) || c->original_status || c->original_spf_size!=t->original_size)
        return abort_open(t);
    out->frozen_revision=t->provider.revision;
    if(stage==2 && t->phase==SHIZUKA_RELAY_SIZE && !c->spf) {
        t->phase=SHIZUKA_RELAY_DRIVER;
        out->advertised_size=t->original_size+SHIZUKA_RELAY_GRAPH_GROWTH;
        return SHIZUKA_RELAY_ADVERTISE;
    }
    if(stage==3 && t->phase==SHIZUKA_RELAY_DRIVER && c->spf
        && c->spf_capacity>=t->original_size+SHIZUKA_RELAY_GRAPH_GROWTH) {
        size_t written=0;
        int status=shizuka_relay_graph_transform(c->spf,t->original_size,s->client_port,
                                               workspace,capacity,&written);
        if(status || written!=t->original_size+SHIZUKA_RELAY_GRAPH_GROWTH)return abort_open(t);
        for(size_t i=0;i<written;i++)c->spf[i]=workspace[i];
        t->phase=SHIZUKA_RELAY_FILLED;out->advertised_size=(uint32_t)written;
        return SHIZUKA_RELAY_COMMITTED;
    }
    return abort_open(t);
}

int shizuka_relay_open_complete(struct shizuka_relay_transaction *t,const struct shizuka_relay_scope *s,int accepted)
{
    if(!t || !s || !same_owner(&t->scope,s))return 0;
    if(t->phase!=SHIZUKA_RELAY_FILLED || accepted!=1) { abort_open(t);return 0; }
    t->phase=SHIZUKA_RELAY_ACTIVE;return 1;
}
void shizuka_relay_close(struct shizuka_relay_transaction *t,const struct shizuka_relay_scope *s)
{
    if(t && s && same_owner(&t->scope,s)) { t->phase=SHIZUKA_RELAY_CLOSED;t->profile_confirmed=0; }
}
void shizuka_relay_reset(struct shizuka_relay_transaction *t)
{
    if(t) { uint8_t *p=(uint8_t *)t;for(size_t i=0;i<sizeof(*t);i++)p[i]=0; }
}
int shizuka_relay_confirm_profile(struct shizuka_relay_transaction *t,const struct shizuka_relay_scope *s,
    uint32_t accepted,uint32_t mid,uint32_t mode,uint32_t rate,uint32_t version,uint32_t lossless)
{
    if(!t || !s || !same_owner(&t->scope,s) || t->phase!=SHIZUKA_RELAY_ACTIVE)return 0;
    t->profile_confirmed=(accepted==1 && mid==0x07001082 && mode==4096 && rate==44100
                          && version==3 && lossless==0);
    return (int)t->profile_confirmed;
}
uint32_t shizuka_relay_active_delta(const struct shizuka_relay_transaction *t)
{
    return t && t->phase==SHIZUKA_RELAY_ACTIVE && t->profile_confirmed ? t->provider.hq_extra_ttp_us:0;
}
