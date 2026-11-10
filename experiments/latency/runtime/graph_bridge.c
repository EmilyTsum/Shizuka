/* SPDX-License-Identifier: GPL-2.0-only */
#include "graph_bridge.h"

static uint32_t get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p)|(uint64_t)get32(p+4)<<32; }
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static void copy_bytes(void *to,const void *from,size_t n)
{ uint8_t *d=to;const uint8_t *s=from;for(size_t i=0;i<n;i++)d[i]=s[i]; }
static int range_valid(uintptr_t p,size_t n,size_t alignment)
{ return p && !(p&(alignment-1)) && n<=UINTPTR_MAX-p; }
static int same_owner(const struct shizuka_relay_scope *a,const struct shizuka_relay_scope *b)
{
    return a->graph==b->graph && a->epoch==b->epoch && a->source_port==b->source_port
        && a->client_port==b->client_port;
}
static int context_ready(const S25LatencyContext *c)
{
    if(c->magic!=S25_LATENCY_CONTEXT_MAGIC || c->abi!=S25_LATENCY_CONTEXT_ABI
        || c->size!=sizeof(*c) || !c->sequence || (c->sequence&1) || c->valid!=1
        || c->sample_rate!=44100 || c->mode!=4096 || c->private_flags!=2)return 0;
    for(unsigned i=0;i<7;i++)if(c->reserved[i])return 0;
    return 1;
}
static int context_same(const struct shizuka_graph_bridge *b,const struct shizuka_graph_bridge_env *e)
{
    return b->frozen_context_seq && context_ready(&e->context)
        && e->context.sequence==b->frozen_context_seq
        && e->context.config_fingerprint==b->frozen_context.config_fingerprint;
}
static int owner_matches(const struct shizuka_graph_bridge *b,const struct shizuka_graph_bridge_env *e)
{
    return e && b->frozen_context_seq && e->library_base==b->library_base
        && same_owner(&b->transaction.scope,&e->scope);
}
static int32_t abort_owned(struct shizuka_graph_bridge *b,struct shizuka_graph_bridge_result *r)
{
    b->transaction.phase=SHIZUKA_RELAY_FAILED;b->transaction.profile_confirmed=0;
    shizuka_relay_calibration_reset(&b->calibration);
    if(r)r->action=SHIZUKA_RELAY_ABORT;
    return SHIZUKA_GRAPH_AR_EFAILED;
}
/* The exact owner supplied these arrays to a successful original ACDB call.
 * Do not cast packed/possibly unaligned command fields into native structs.
 */
static int decode_ids(const uint8_t *p,uint32_t n,uint32_t *out)
{
    if(!n || n>SHIZUKA_GSL_MAX_SGIDS || !range_valid((uintptr_t)p,(size_t)n*4,4))return 0;
    for(uint32_t i=0;i<n;i++)out[i]=get32(p+(size_t)i*4);
    return 1;
}
static int decode_keys(const uint8_t *p,uint32_t n,struct shizuka_gsl_key *out)
{
    if(n>SHIZUKA_GSL_MAX_KEYS || (n && !range_valid((uintptr_t)p,(size_t)n*8,4)))return 0;
    for(uint32_t i=0;i<n;i++) {out[i].key=get32(p+(size_t)i*8);out[i].value=get32(p+(size_t)i*8+4);}
    return 1;
}
static const uint8_t *pointer_at(const uint8_t *p)
{
    uint64_t v=get64(p);
    return v>UINTPTR_MAX?0:(const uint8_t *)(uintptr_t)v;
}
static int graph_site(uint32_t pc)
{ return pc==0x246e4 || pc==0x247d0 || pc==0x24e20; }
static int cal_site(uint32_t pc)
{ return pc==0x1e1c0 || pc==0x1e2d8; }

int32_t shizuka_graph_bridge_after_acdb(struct shizuka_graph_bridge *b,
    const struct shizuka_graph_bridge_env *e,uint32_t opcode,const void *command,
    uint32_t command_size,void *response,uint32_t response_size,int32_t original_status,
    uintptr_t graph,uint32_t pc,uint8_t *workspace,size_t workspace_capacity,
    struct shizuka_graph_bridge_result *result)
{
    struct shizuka_graph_bridge_result ignored;
    if(!result)result=&ignored;
    result->action=SHIZUKA_RELAY_BYPASS;result->actual_size=0;result->removed=0;
    if(!b || !e)return original_status;
    int is_graph=opcode==SHIZUKA_GRAPH_ACDB_DATA && graph_site(pc);
    int is_cal=opcode==SHIZUKA_GRAPH_ACDB_CAL && cal_site(pc);
    if(!is_graph && !is_cal)return original_status;
    /* A call for another graph is never allowed to abort or alter this owner. */
    if(b->frozen_context_seq && graph!=b->transaction.scope.graph)return original_status;
    int owned=b->frozen_context_seq && graph==b->transaction.scope.graph;
    if(!graph || graph!=e->scope.graph || !e->library_base
        || !(e->scope.flags&SHIZUKA_GSL_EXACT_LIBRARY))
        return owned?abort_owned(b,result):original_status;
    if(owned && !owner_matches(b,e))return abort_owned(b,result);
    if(original_status && is_graph) {
        if(owned)abort_owned(b,result);
        return original_status;
    }
    if(command_size!=(is_graph?24U:36U) || response_size!=(is_graph?28U:12U)
        || !range_valid((uintptr_t)command,command_size,1)
        || !range_valid((uintptr_t)response,response_size,1))
        return original_status?original_status:(owned?abort_owned(b,result):original_status);
    const uint8_t *cmd=command;uint8_t *rsp=response;
    uint32_t sgids[SHIZUKA_GSL_MAX_SGIDS];
    struct shizuka_gsl_key keys[SHIZUKA_GSL_MAX_KEYS],keys2[SHIZUKA_GSL_MAX_KEYS];
    uint32_t num_sgids=get32(cmd),num_keys=get32(cmd+12);
    if(!decode_ids(pointer_at(cmd+4),num_sgids,sgids))
        return original_status?original_status:(owned?abort_owned(b,result):original_status);
    if(is_cal){
        uint32_t target=0;for(uint32_t i=0;i<num_sgids;++i)target+=sgids[i]==SHIZUKA_RELAY_SG;
        if(!target)return original_status;
        if(original_status){if(owned)shizuka_relay_calibration_reset(&b->calibration);return original_status;}
    }
    if(!decode_keys(pointer_at(cmd+16),num_keys,keys))
        return owned?abort_owned(b,result):original_status;

    if(is_graph) {
        if(owned && !context_same(b,e))return abort_owned(b,result);
        /* No graph field or setting can replace the missing true mode tag. */
        if(!owned && (!context_ready(&e->context) || e->transport_ready!=1))return original_status;
        struct shizuka_relay_scope scope=e->scope;
        if(e->transport_ready==1)scope.flags|=SHIZUKA_GSL_TRANSPORT_READY;
        else scope.flags&=~SHIZUKA_GSL_TRANSPORT_READY;
        uint32_t size=get32(rsp+16);
        uint8_t *spf=(uint8_t *)(uintptr_t)pointer_at(rsp+20);
        result->actual_size=size;
        if((pc!=0x24e20 && spf) || (pc==0x24e20 && (!spf
            || !range_valid((uintptr_t)spf,(size_t)size+SHIZUKA_RELAY_GRAPH_GROWTH,4))))
            return owned?abort_owned(b,result):original_status;
        struct shizuka_gsl_after_call call={pc,opcode,command_size,response_size,
            original_status,size,sgids,num_sgids,keys,num_keys,spf,e->spf_capacity};
        struct shizuka_relay_after_result_data out;
        int action=shizuka_relay_after_acdb(&b->transaction,&scope,&e->provider,&call,
                                          workspace,workspace_capacity,&out);
        result->action=action;
        if(action==SHIZUKA_RELAY_ABORT)return abort_owned(b,result);
        if(action==SHIZUKA_RELAY_ADVERTISE || action==SHIZUKA_RELAY_COMMITTED) {
            if(!b->frozen_context_seq) {
                b->frozen_context=e->context;b->frozen_context_seq=e->context.sequence;
                b->library_base=e->library_base;b->open_accepted=0;
                shizuka_relay_calibration_reset(&b->calibration);
            }
            put32(rsp+16,out.advertised_size);result->actual_size=out.advertised_size;
        }
        return original_status;
    }

    /* Replaced RD/WR nodes still require logger calibration removal after a
     * provider expires or mode/context is revoked. Only a destroyed/other epoch
     * loses this ownership proof. It is separate from positive-delta eligibility.
     */
    if(!owned || !b->open_accepted)return original_status;
    uint32_t num_new=get32(cmd+24);
    if(!decode_keys(pointer_at(cmd+28),num_new,keys2)
        || e->num_gkv>SHIZUKA_GSL_MAX_KEYS
        || (e->num_gkv && !range_valid((uintptr_t)e->current_gkv,(size_t)e->num_gkv*8,4)))
        return abort_owned(b,result);
    uint32_t size=get32(rsp);uint8_t *data=(uint8_t *)(uintptr_t)pointer_at(rsp+4);
    result->actual_size=size;
    if(size>SHIZUKA_RELAY_MAX_SPF || (size && data && !range_valid((uintptr_t)data,size,4)))
        return abort_owned(b,result);
    /* The owner GKV is a native aligned view supplied by the embedding owner. */
    struct shizuka_gsl_cal_call call={pc,opcode,command_size,response_size,original_status,
        sgids,num_sgids,e->current_gkv,e->num_gkv,keys,keys2,num_keys,num_new,
        data,size,e->cal_capacity};
    struct shizuka_relay_cal_result out;
    struct shizuka_relay_transaction cal_owner;
    copy_bytes(&cal_owner,&b->transaction,sizeof cal_owner);
    cal_owner.phase=SHIZUKA_RELAY_ACTIVE;
    int action=shizuka_relay_after_calibration(&cal_owner,&b->calibration,&e->scope,&call,&out);
    result->action=action;result->removed=out.removed;
    if(action==SHIZUKA_RELAY_ABORT)return abort_owned(b,result);
    /* GSL can skip fill altogether for a successful zero-length query. Do not
     * leave a phantom pending query that would reject its next calibration.
     */
    if(action==SHIZUKA_RELAY_BYPASS && pc==0x1e1c0 && !size)
        shizuka_relay_calibration_reset(&b->calibration);
    if(action==SHIZUKA_RELAY_COMMITTED || action==SHIZUKA_RELAY_CAL_EMPTY) {
        put32(rsp,out.actual_size);result->actual_size=out.actual_size;
        if(action==SHIZUKA_RELAY_CAL_EMPTY)return SHIZUKA_GRAPH_AR_ENOTEXIST;
    }
    return original_status;
}

int shizuka_graph_bridge_open_ack(struct shizuka_graph_bridge *b,
    const struct shizuka_graph_bridge_env *e,uint32_t opcode,uint32_t port,int32_t status)
{
    if(!b || !owner_matches(b,e) || opcode!=SHIZUKA_GRAPH_OPEN_OPCODE
        || port!=b->transaction.scope.source_port)return 0;
    if(!context_same(b,e) || e->transport_ready!=1 || !(e->scope.flags&SHIZUKA_GSL_SOURCE_REGISTERED) || status) {
        abort_owned(b,0);return 0;
    }
    if(!shizuka_relay_open_complete(&b->transaction,&e->scope,1))return 0;
    b->open_accepted=1;shizuka_relay_calibration_reset(&b->calibration);return 1;
}
int shizuka_graph_bridge_confirm_profile(struct shizuka_graph_bridge *b,
    const struct shizuka_graph_bridge_env *e,uint32_t accepted,uint32_t mid,
    uint32_t mode,uint32_t rate,uint32_t strict_r3_success)
{
    if(!b || !owner_matches(b,e))return 0;
    if(!context_same(b,e) || e->transport_ready!=1 || !(e->scope.flags&SHIZUKA_GSL_SOURCE_REGISTERED)) {abort_owned(b,0);return 0;}
    return shizuka_relay_confirm_profile(&b->transaction,&e->scope,accepted,mid,mode,rate,
                                        strict_r3_success==1?3U:0U,0);
}
uint32_t shizuka_graph_bridge_active_delta(const struct shizuka_graph_bridge *b,
    const struct shizuka_graph_bridge_env *e)
{
    return b && owner_matches(b,e) && context_same(b,e) && e->transport_ready==1 && (e->scope.flags&SHIZUKA_GSL_SOURCE_REGISTERED)
        ?shizuka_relay_active_delta(&b->transaction):0;
}
void shizuka_graph_bridge_reset(struct shizuka_graph_bridge *b)
{ if(b) {uint8_t *p=(uint8_t *)b;for(size_t i=0;i<sizeof(*b);i++)p[i]=0;} }
void shizuka_graph_bridge_close(struct shizuka_graph_bridge *b,const struct shizuka_relay_scope *s)
{
    if(b && s && b->frozen_context_seq && same_owner(&b->transaction.scope,s))
        shizuka_graph_bridge_reset(b);
}
