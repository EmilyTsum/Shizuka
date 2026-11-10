/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200112L
#include "shizuka/hal_transport.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SENT 512u
struct sent { struct shizuka_transport_packet packet; uint8_t payload[1056]; };
struct fake {
    void *context;
    shizuka_transport_receive_fn receive;
    void *receive_context;
    struct sent sent[MAX_SENT];
    uint32_t sent_count, registered, unregistered, allocated, freed, sync_start, sync_end;
    uint32_t transforms, fail_opcode, fail_send, suppress_opcode, unmap_error, transform_error, output_md_bytes;
    uint32_t next_handle, registration_tokens[8];
    uint32_t active_wr_token, parser_extra_flags;
    uint32_t discards, discard_error, suppress_frame_size, suppress_media_format, suppress_upstream_state;
    uint32_t prestart_format_empty;
    uint8_t *buffers[2];
    uint64_t now;
};
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void write32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void deliver(struct fake *f, uint32_t peer, uint32_t token, uint32_t opcode, const uint8_t *data, uint32_t bytes)
{
    struct shizuka_transport_packet p = {0};
    p.src_domain=2; p.dst_domain=3; p.src_port=peer; p.dst_port=SHIZUKA_TRANSPORT_CLIENT_PORT;
    p.token=token; p.opcode=opcode; p.payload=data; p.payload_bytes=bytes;
    assert(f->receive(f->receive_context,&p)==0);
}
static void basic(struct fake *f, const struct shizuka_transport_packet *p, uint32_t status)
{
    uint8_t body[8]; write32(body,p->opcode); write32(body+4,status);
    deliver(f,p->dst_port,p->token,0x02001005u,body,8);
}
static int32_t port_register(void *u,uint32_t port,shizuka_transport_receive_fn cb,void *ctx)
{
    struct fake *f=u; assert(port==SHIZUKA_TRANSPORT_CLIENT_PORT); assert(!f->registered);
    f->registered=1;f->receive=cb;f->receive_context=ctx;return 0;
}
static int32_t port_unregister(void *u,uint32_t port)
{
    struct fake *f=u;assert(port==SHIZUKA_TRANSPORT_CLIENT_PORT);assert(f->registered);
    f->registered=0;f->unregistered++;return 0;
}
static uint32_t event_index(uint32_t event)
{
    const uint32_t events[8]={0x06001000u,0x06001001u,0x03001004u,0x03001006u,0x03001007u,0x06001003u,0x03001009u,0x03001005u};
    uint32_t i;for(i=0;i<8;i++)if(events[i]==event)return i;assert(0);return 0;
}
static int32_t send_packet(void *u,const struct shizuka_transport_packet *p)
{
    struct fake *f=u;struct sent *s;uint8_t response[16]={0};
    assert(f->registered);assert(p->src_domain==3&&p->dst_domain==2);
    assert(p->src_port==SHIZUKA_TRANSPORT_CLIENT_PORT);assert(p->payload_bytes<=1056);
    assert(f->sent_count<MAX_SENT);s=&f->sent[f->sent_count++];s->packet=*p;
    if(p->payload_bytes)memcpy(s->payload,p->payload,p->payload_bytes);s->packet.payload=s->payload;
    if(p->opcode==f->fail_opcode)return (int32_t)f->fail_send;
    if(p->opcode==f->suppress_opcode)return 0;
    switch(p->opcode){
    case 0x01001021u:
        assert(p->dst_port==1&&p->payload_bytes==0);write32(response,1);
        deliver(f,1,p->token,0x02001007u,response,4);break;
    case 0x0100100cu:
        assert(p->dst_port==1&&p->client_data==1&&p->payload_bytes==20);
        assert(read32(p->payload)==0x00010003u&&read32(p->payload+4)==4);
        assert(read32(p->payload+8)>=7&&read32(p->payload+12)==0&&read32(p->payload+16)==16384);
        write32(response,++f->next_handle);deliver(f,1,p->token,0x02001001u,response,4);break;
    case 0x0100100eu:{
        uint32_t event;assert(p->payload_bytes==40&&read32(p->payload+12)==24);
        assert(read32(p->payload+16)==p->dst_port&&read32(p->payload+24)==1);
        event=read32(p->payload+20);f->registration_tokens[event_index(event)]=p->token;
        /* A valid event may precede its own REGISTER basic ACK and shares its
         * token. It must not be misclassified as that control response. */
        if(event==0x03001009u&&!f->suppress_media_format){write32(response,6);write32(response+4,0x13204);write32(response+8,0);
            deliver(f,p->dst_port,p->token,event,response,12);}
        basic(f,p,0);break;}
    case 0x01001006u:
        assert(p->payload_bytes%8==0&&read32(p->payload+12)==p->payload_bytes-16);
        assert(read32(p->payload+16)==p->dst_port&&read32(p->payload+20)==0x080011c3u);
        /* Audit actual outgoing endpoint semantics, not just the outer ABI.
         * Type3 to WR aliases its external-input queue bit mask in firmware. */
        {
            uint32_t count=read32(p->payload+32),bytes=read32(p->payload+24);
            assert(count&&count<=8&&bytes==4+8*count&&p->payload_bytes==32+((bytes+7)&~7u));
            for(uint32_t i=0;i<count;i++){
                uint32_t type=read32(p->payload+36+8*i),value=read32(p->payload+40+8*i);
                if(p->dst_port==SHIZUKA_TRANSPORT_WR_MIID)assert(type==1&&value<=1);
                else {assert(p->dst_port==SHIZUKA_TRANSPORT_RD_MIID);
                    assert((type==2&&value<=1)||(type==3&&value<=3));}
            }
        }
        basic(f,p,0);
        if(p->dst_port==SHIZUKA_TRANSPORT_RD_MIID&&read32(p->payload+32)==1&&
            read32(p->payload+36)==3&&
            (read32(p->payload+40)==1||read32(p->payload+40)==2)){
            write32(response,SHIZUKA_TRANSPORT_RD_MIID);write32(response+4,1);write32(response+8,3);write32(response+12,2);
            if(read32(p->payload+40)==2&&!f->suppress_upstream_state)deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[4],0x03001007u,response,16);
            write32(response,1024);write32(response+4,1);write32(response+8,SHIZUKA_TRANSPORT_RD_MIID);
            if(!f->suppress_frame_size)deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[2],0x03001004u,response,12);
        }break;
    case 0x04001001u:
        assert(p->dst_port==SHIZUKA_TRANSPORT_WR_MIID&&p->payload_bytes==12);
        assert(read32(p->payload)==6&&read32(p->payload+4)==0x13204);basic(f,p,0);break;
    case 0x0100100du:
        assert(p->dst_port==1&&p->client_data==0&&p->payload_bytes==4&&read32(p->payload)>100);
        basic(f,p,f->unmap_error);break;
    case 0x0400100bu:
        assert(p->dst_port==SHIZUKA_TRANSPORT_RD_MIID&&p->payload_bytes==32);
        if(f->prestart_format_empty) {
            uint8_t empty[56]={0};f->prestart_format_empty=0;
            write32(response,6);write32(response+4,0x13204);
            deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[6],0x03001009u,response,12);
            write32(empty+12,read32(p->payload+8));write32(empty+20,0xdeadbeefu);
            write32(empty+40,4096);write32(empty+48,read32(p->payload+8));
            deliver(f,SHIZUKA_TRANSPORT_RD_MIID,p->token,0x05001005u,empty,56);
        }break;
    case 0x0400100au:
        assert(p->dst_port==SHIZUKA_TRANSPORT_WR_MIID&&p->payload_bytes==44);
        assert(!(read32(p->payload+24)&~0x80000000u)); /* No invented EOF/continue. */
        assert(!f->active_wr_token);f->active_wr_token=p->token;break;
    default:assert(0);}
    return 0;
}
static int32_t allocate_memory(void *u,struct shizuka_transport_memory *m)
{
    struct fake *f=u;void *p=0;assert(f->allocated<2);assert(m->subsystem_id==2);
    assert(!posix_memalign(&p,64,16384));memset(p,0,16384);f->buffers[f->allocated]=p;
    m->cpu_address=p;m->capacity=16384;m->fd=(int32_t)(7+f->allocated++);m->offset_mode=1;return 0;
}
static int32_t free_memory(void *u,struct shizuka_transport_memory *m)
{
    struct fake *f=u;assert(m->cpu_address);free(m->cpu_address);m->cpu_address=0;f->freed++;return 0;
}
static int32_t cpu_sync(void *u,struct shizuka_transport_memory *m,uint32_t begin)
{
    struct fake *f=u;assert(m->cpu_address);if(begin)f->sync_start++;else f->sync_end++;return 0;
}
static int32_t transform(void *u,uint32_t profile,uint64_t delta,uint8_t *data,uint32_t bytes,uint32_t cap,
    uint8_t *md,uint32_t md_bytes,uint32_t md_cap,struct shizuka_transport_transform_result *out)
{
    struct fake *f=u;uint32_t supplemental=data[1];
    assert(profile==1&&delta==150000);assert(bytes==32&&cap==4096);
    assert(md==data+4096&&md_bytes==(supplemental?68u:40u)&&md_cap==4096);assert(data[0]==1);
    if(f->transform_error)return -1;
    data[0]=2;out->data_bytes=32;out->metadata_bytes=supplemental?28:f->output_md_bytes;
    out->timestamp_bits=supplemental?0:42;out->wr_flags=(supplemental?0x10u:0x80000010u)|f->parser_extra_flags;
    out->wrappers_changed=supplemental?0:1;f->transforms++;return 0;
}
static uint64_t clock_ms(void *u){return ((struct fake*)u)->now;}
static int32_t validate_discard(void *u,uint32_t profile,uint8_t *data,uint32_t bytes,uint32_t cap,
    uint8_t *md,uint32_t md_bytes,uint32_t md_cap)
{
    struct fake *f=u;assert(profile==1&&data[0]==1&&bytes==32&&cap==4096);
    assert(md==data+4096&&md_bytes==40&&md_cap==4096);f->discards++;return f->discard_error ? -1 : 0;
}
static struct shizuka_transport_status status(struct fake *f)
{
    struct shizuka_transport_status s={0};s.abi=1;s.bytes=sizeof(s);
    assert(!shizuka_transport_get_status(f->context,&s));return s;
}
static void create(struct fake *f)
{
    struct shizuka_transport_host h={0};struct shizuka_transport_options o={0};
    memset(f,0,sizeof(*f));f->next_handle=100;f->now=10;f->context=calloc(1,shizuka_transport_context_bytes());assert(f->context);
    h.abi=1;h.bytes=sizeof(h);h.user=f;h.register_port=port_register;h.unregister_port=port_unregister;h.send=send_packet;
    h.allocate=allocate_memory;h.free=free_memory;h.cpu_sync=cpu_sync;h.transform=transform;h.clock_ms=clock_ms;
    h.validate_discard=validate_discard;
    o.abi=1;o.bytes=sizeof(o);o.token_seed=0x12340000;o.control_timeout_ms=1000;o.buffer_timeout_ms=2000;
    o.hal_generation=11;o.dsp_generation=12;
    assert(!shizuka_transport_init(f->context,shizuka_transport_context_bytes(),&h,&o));assert(!shizuka_transport_attach(f->context));
}
static void pump(struct fake *f,uint32_t count){uint32_t i;for(i=0;i<count;i++)(void)shizuka_transport_step(f->context);}
static void bind_epoch(struct fake *f,uint32_t attested,uint64_t epoch)
{
    struct shizuka_transport_graph g={0};pump(f,3);
    assert(status(f).registration_ready||status(f).phase==SHIZUKA_TRANSPORT_CLOSED);
    g.abi=1;g.bytes=sizeof(g);g.graph_epoch=epoch;g.hal_generation=11;g.dsp_generation=12;g.delta_us=150000;g.profile=1;
    g.subgraph_id=0xb00000ab;g.container_id=0xe00000e6;g.parent_client_id=SHIZUKA_TRANSPORT_CLIENT_PORT;
    g.rd_miid=SHIZUKA_TRANSPORT_RD_MIID;g.wr_miid=SHIZUKA_TRANSPORT_WR_MIID;
    g.fresh_owned_graph=g.endpoint_namespace_owned=g.original_imcl_preserved=g.olc_parent_protocol_reviewed=1;
    g.actual_init_profile_attested=attested;g.open_acknowledged=1;
    assert(!shizuka_transport_bind_graph(f->context,&g));
}
static void bind(struct fake *f,uint32_t attested){bind_epoch(f,attested,1);}
static uint32_t count_opcode(struct fake *f,uint32_t opcode)
{uint32_t i,n=0;for(i=0;i<f->sent_count;i++)if(f->sent[i].packet.opcode==opcode)n++;return n;}
static const struct sent *nth_opcode(struct fake *f,uint32_t opcode,uint32_t n)
{uint32_t i;for(i=0;i<f->sent_count;i++)if(f->sent[i].packet.opcode==opcode){if(!n--)return &f->sent[i];}assert(0);return 0;}
static void start(struct fake *f)
{
    pump(f,30);assert(status(f).configuration_ready);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,20);
}
static void close_graph(struct fake *f)
{
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPING,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_CLOSED,0));
    f->active_wr_token=0;pump(f,12);
}
static void destroy(struct fake *f)
{
    assert(!shizuka_transport_detach(f->context));assert(f->registered==0&&f->unregistered==1);
    free(f->context);f->context=0;
}
static void test_media_format_observation(struct fake *f)
{
    uint8_t format[16]={0};uint32_t token;
    create(f);f->suppress_media_format=1;bind(f,0);pump(f,30);
    assert(status(f).configuration_ready&&!status(f).media_format_bytes&&!status(f).error);
    token=f->registration_tokens[6];assert(token);
    write32(format,6);write32(format+4,0x13204);
    /* Only the registered RD event's own token and opcode authorize a format
     * observation. Unrelated callback traffic must leave the diagnostic zero. */
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,token^1u,0x03001009u,format,12);pump(f,1);
    assert(!status(f).media_format_bytes&&!status(f).error);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,token,0x03001999u,format,12);pump(f,1);
    assert(!status(f).media_format_bytes&&!status(f).error);
    deliver(f,SHIZUKA_TRANSPORT_WR_MIID,token,0x03001009u,format,12);pump(f,1);
    assert(!status(f).media_format_bytes&&!status(f).error);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,token,0x03001009u,format,12);pump(f,2);
    assert(status(f).media_format_bytes==12&&status(f).media_format_ready&&!status(f).error);
    assert(count_opcode(f,0x04001001u)==1);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,token,0x03001009u,format,12);pump(f,1);
    assert(status(f).media_format_bytes==12&&count_opcode(f,0x04001001u)==1);
    write32(format+8,4);write32(format+12,0x12345678);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,token^1u,0x03001009u,format,16);pump(f,1);
    assert(status(f).media_format_bytes==12&&!status(f).error);
    close_graph(f);destroy(f);

    create(f);f->suppress_media_format=1;bind(f,0);pump(f,30);
    assert(!status(f).media_format_bytes);
    token=f->registration_tokens[6];write32(format,5);write32(format+8,0);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,token,0x03001009u,format,12);pump(f,1);
    assert(status(f).error&&status(f).need_graph_close&&!status(f).media_format_bytes);
    close_graph(f);destroy(f);
}
static void rd_done_kind(struct fake *f,const struct sent *read,uint32_t corrupt,uint32_t supplemental)
{
    uint8_t b[56]={0};uint32_t handle=read32(read->payload+8);uint32_t index=handle-101;
    assert(index<2);f->buffers[index][0]=1;f->buffers[index][1]=(uint8_t)supplemental;
    write32(b+12,handle+corrupt);write32(b+16,32);
    write32(b+20,supplemental?0:42);write32(b+28,supplemental?0:0x80000000u);
    write32(b+32,1);write32(b+40,4096);write32(b+48,handle);write32(b+52,supplemental?68:40);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,read->packet.token,0x05001005u,b,sizeof(b));
}
static void rd_done(struct fake *f,const struct sent *read,uint32_t corrupt)
{rd_done_kind(f,read,corrupt,0);}
static void wr_done(struct fake *f,const struct sent *write)
{
    uint8_t b[32]={0};write32(b+8,read32(write->payload+8));
    write32(b+16,read32(write->payload+28));write32(b+20,read32(write->payload+32));
    write32(b+24,read32(write->payload+36));
    if(f->active_wr_token==write->packet.token)f->active_wr_token=0;
    deliver(f,SHIZUKA_TRANSPORT_WR_MIID,write->packet.token,0x05001004u,b,sizeof(b));
}
static void rd_empty_done(struct fake *f,const struct sent *read)
{
    uint8_t b[56]={0};uint32_t handle=read32(read->payload+8);
    write32(b+12,handle);write32(b+40,4096);write32(b+48,handle);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,read->packet.token,0x05001005u,b,sizeof(b));
}
static void startup_media_format(struct fake *f)
{
    uint8_t body[12]={0};write32(body,6);write32(body+4,0x13204);
    /* Match real OLC ordering: RD buffers are already posted when the first
     * media-format event arrives. No reg-time synthetic format is emitted. */
    assert(count_opcode(f,0x0400100bu)==2);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[6],0x03001009u,body,sizeof body);
}
static void startup_without_format(struct fake *f)
{
    create(f);f->suppress_media_format=1;bind(f,1);start(f);
    assert(status(f).configuration_ready&&!status(f).media_format_ready);
    assert(status(f).phase==SHIZUKA_TRANSPORT_CONFIGURED&&!status(f).error);
    assert(count_opcode(f,0x0400100bu)==2&&!count_opcode(f,0x0400100au));
    assert(!count_opcode(f,0x04001001u)&&!f->transforms&&status(f).unresolved_buffers==2);
}
static void test_rd_bootstrap_before_media_format(struct fake *f)
{
    startup_without_format(f);
    /* Wrong token cannot return a lease or cause WR, even after format ACK. */
    struct sent wrong=*nth_opcode(f,0x0400100bu,0);wrong.packet.token+=0x10000;
    rd_done(f,&wrong,0);pump(f,1);
    assert(status(f).unresolved_buffers==2&&!f->transforms&&!count_opcode(f,0x0400100au));
    rd_done(f,nth_opcode(f,0x0400100bu,1),0);pump(f,1);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));
    f->suppress_opcode=0x04001001u;
    startup_media_format(f);pump(f,2);
    const struct sent *format=nth_opcode(f,0x04001001u,0);
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    assert(!status(f).media_format_ready&&status(f).media_format_bytes==12);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));
    uint8_t ack[8];write32(ack,0x04001001u);write32(ack+4,0);
    deliver(f,SHIZUKA_TRANSPORT_WR_MIID,format->packet.token+1,0x02001005u,ack,8);pump(f,1);
    assert(!status(f).media_format_ready&&!f->transforms&&!count_opcode(f,0x0400100au));
    basic(f,&format->packet,0);pump(f,1);
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&status(f).media_format_ready);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1);
    assert(read32(nth_opcode(f,0x0400100au,0)->payload+8)==101);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,1);
    assert(f->transforms==2&&count_opcode(f,0x0400100au)==2);
    assert(read32(nth_opcode(f,0x0400100au,1)->payload+8)==102);
    close_graph(f);assert(f->freed==2);destroy(f);

    /* A missing format cannot be bypassed by receiving a complete frame. The
     * original activation deadline still closes this bounded bootstrap. */
    startup_without_format(f);rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    f->now+=1001;pump(f,1);
    assert(status(f).error&&status(f).need_graph_close&&!f->transforms&&!count_opcode(f,0x0400100au));
    close_graph(f);assert(f->freed==2);destroy(f);

    /* CLOSE owns all leases, including exact callbacks parked awaiting format.
     * No temporary WR/transform is needed to retire those buffers. */
    startup_without_format(f);
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);rd_done(f,nth_opcode(f,0x0400100bu,1),0);pump(f,1);
    assert(status(f).unresolved_buffers==2&&!f->transforms);
    close_graph(f);assert(f->freed==2&&count_opcode(f,0x0100100du)==2);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));destroy(f);

    /* A normal STOP drains parked frames through the unchanged strict discard
     * validator, preserves mappings, and never publishes rewrite telemetry. */
    startup_without_format(f);
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);rd_empty_done(f,nth_opcode(f,0x0400100bu,1));pump(f,1);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPED,0));pump(f,1);
    assert(status(f).phase==SHIZUKA_TRANSPORT_PAUSED&&f->discards==1);
    assert(!f->transforms&&!count_opcode(f,0x0400100au)&&!f->freed);
    close_graph(f);assert(f->freed==2);destroy(f);
}
static void state_event(struct fake *f,uint32_t state,uint32_t wrong_token)
{
    uint8_t b[16];write32(b,SHIZUKA_TRANSPORT_RD_MIID);write32(b+4,1);write32(b+8,3);write32(b+12,state);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[4]+wrong_token,0x03001007u,b,sizeof b);
}
static void property_event(struct fake *f,uint32_t opcode,const uint32_t *pairs,uint32_t count)
{
    uint8_t b[72]={0};uint32_t peer=opcode==0x03001005u?SHIZUKA_TRANSPORT_WR_MIID:SHIZUKA_TRANSPORT_RD_MIID;
    assert(count<=8);write32(b,peer);write32(b+4,count);
    for(uint32_t i=0;i<2*count;i++)write32(b+8+4*i,pairs[i]);
    deliver(f,peer,f->registration_tokens[event_index(opcode)],opcode,b,8+8*count);
}
static void test_wr_peer_observation(struct fake *f)
{
    for(uint32_t value=0;value<=1;value++){
        create(f);bind(f,1);start(f);
        struct shizuka_transport_status s=status(f);
        assert(!s.wr_peer_bytes&&!s.wr_peer_count&&!s.wr_peer_type&&!s.wr_peer_value);
        uint32_t before=count_opcode(f,0x01001006u),pairs[]={1,value};
        property_event(f,0x03001006u,pairs,1);pump(f,3);s=status(f);
        assert(s.wr_peer_bytes==12&&s.wr_peer_count==1&&s.wr_peer_type==1&&s.wr_peer_value==value);
        assert(count_opcode(f,0x01001006u)==before+1);
        property_event(f,0x03001006u,pairs,1);pump(f,2);s=status(f);
        assert(count_opcode(f,0x01001006u)==before+1&&s.wr_peer_bytes==12&&s.wr_peer_value==value);
        /* A matching endpoint with a wrong event token is not an observation. */
        uint8_t b[16];write32(b,SHIZUKA_TRANSPORT_RD_MIID);write32(b+4,1);
        write32(b+8,1);write32(b+12,!value);
        deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[3]+1,0x03001006u,b,16);pump(f,2);
        assert(status(f).wr_peer_value==value&&!status(f).need_graph_close);
        /* Revocation preserves the current epoch's observation for diagnosis;
         * it does not make its queued payload alternate stream authority. */
        assert(shizuka_transport_attest_profile(f->context,1,2,1)<0);s=status(f);
        assert(s.need_graph_close&&s.wr_peer_bytes==12&&s.wr_peer_value==value);
        close_graph(f);s=status(f);assert(s.wr_peer_bytes==12&&s.wr_peer_value==value);
        bind_epoch(f,1,2);s=status(f);
        assert(s.graph_epoch==2&&!s.wr_peer_bytes&&!s.wr_peer_count&&!s.wr_peer_type&&!s.wr_peer_value);
        assert(!shizuka_transport_graph_stage(f->context,2,SHIZUKA_GRAPH_STOPPING,0));
        assert(!shizuka_transport_graph_stage(f->context,2,SHIZUKA_GRAPH_CLOSED,0));pump(f,12);destroy(f);
    }
    /* The appended16B status contract remains exact: an old-sized output is
     * rejected before any output mutation. */
    create(f);struct shizuka_transport_status s=status(f),saved;
    s.bytes-=16;saved=s;
    assert(shizuka_transport_get_status(f->context,&s)==SHIZUKA_TRANSPORT_BAD_ARGUMENT);
    assert(!memcmp(&s,&saved,sizeof(s)));destroy(f);
}
static void test_directional_peer_protocol(struct fake *f)
{
    /* Neither START nor RD's optional STARTED event invents WR input state. */
    create(f);bind(f,1);start(f);
    assert(count_opcode(f,0x01001006u)==1&&status(f).initial_peer_acks==1);
    const struct sent *seed=nth_opcode(f,0x01001006u,0);
    assert(seed->packet.dst_port==SHIZUKA_TRANSPORT_RD_MIID);
    assert(read32(seed->payload+32)==1&&read32(seed->payload+36)==3&&read32(seed->payload+40)==2);
    state_event(f,2,0);pump(f,2);assert(count_opcode(f,0x01001006u)==1);
    close_graph(f);destroy(f);

    /* A missing seed ACK cannot be replaced by OPFS or the state event.
     * No WR peer command is necessary to complete genuine RD bootstrap. */
    create(f);bind(f,1);f->suppress_opcode=0x01001006u;start(f);
    assert(!status(f).initial_peer_acks&&!count_opcode(f,0x0400100bu));
    seed=nth_opcode(f,0x01001006u,0);state_event(f,2,0);
    uint8_t opfs[12];write32(opfs,1024);write32(opfs+4,1);write32(opfs+8,SHIZUKA_TRANSPORT_RD_MIID);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[2],0x03001004u,opfs,sizeof opfs);pump(f,1);
    assert(!count_opcode(f,0x0400100bu));
    f->suppress_opcode=0;basic(f,&seed->packet,0);pump(f,2);
    assert(status(f).initial_peer_acks==1&&count_opcode(f,0x0400100bu)==2);
    close_graph(f);destroy(f);

    /* Observed upstream RT goes only to WR. Downstream RT/state goes only
     * to RD. Hold completed reads until BOTH actually queued updates ACK. */
    create(f);bind(f,1);start(f);f->suppress_opcode=0x01001006u;
    const uint32_t upstream[]={1,1},downstream[]={2,1,3,2};
    property_event(f,0x03001006u,upstream,1);
    property_event(f,0x03001005u,downstream,2);pump(f,1);
    const struct sent *first=nth_opcode(f,0x01001006u,1);
    assert(first->packet.dst_port==SHIZUKA_TRANSPORT_WR_MIID&&read32(first->payload+36)==1);
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);rd_done(f,nth_opcode(f,0x0400100bu,1),0);pump(f,1);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));
    struct sent wrong=*first;wrong.packet.token++;basic(f,&wrong.packet,0);pump(f,1);
    assert(!f->transforms&&count_opcode(f,0x01001006u)==2);
    property_event(f,0x03001006u,upstream,1);basic(f,&first->packet,0);pump(f,1);
    const struct sent *second=nth_opcode(f,0x01001006u,2);
    assert(second->packet.dst_port==SHIZUKA_TRANSPORT_RD_MIID&&read32(second->payload+32)==2);
    assert(read32(second->payload+36)==2&&read32(second->payload+44)==3);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));
    basic(f,&second->packet,0);pump(f,1);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1&&count_opcode(f,0x01001006u)==3);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,1);
    assert(f->transforms==2&&count_opcode(f,0x0400100au)==2);
    property_event(f,0x03001006u,upstream,1);property_event(f,0x03001005u,downstream,2);pump(f,2);
    assert(count_opcode(f,0x01001006u)==3);close_graph(f);destroy(f);

    /* Validate the complete list before forwarding: even a valid first pair
     * must not escape when a later pair has the wrong endpoint direction. */
    const struct {uint32_t opcode,count,pairs[4];} invalid[]={
        {0x03001006u,1,{2,1}}, {0x03001006u,1,{3,2}},
        {0x03001006u,1,{4,0}}, {0x03001006u,1,{5,1}},
        {0x03001006u,1,{1,2}}, {0x03001006u,2,{1,1,3,2}},
        {0x03001005u,1,{1,1}}, {0x03001005u,1,{4,0}},
        {0x03001005u,1,{5,1}}, {0x03001005u,1,{2,2}},
        {0x03001005u,1,{3,4}}, {0x03001005u,2,{2,1,1,1}},
        {0x03001007u,1,{1,1}}, {0x03001007u,2,{3,2,3,2}}
    };
    for(uint32_t i=0;i<sizeof invalid/sizeof(*invalid);i++){
        create(f);bind(f,1);start(f);
        uint32_t states=status(f).rd_state_events;
        property_event(f,invalid[i].opcode,invalid[i].pairs,invalid[i].count);pump(f,2);
        assert(status(f).error&&status(f).need_graph_close&&count_opcode(f,0x01001006u)==1);
        assert(status(f).rd_state_events==states&&!f->transforms&&!count_opcode(f,0x0400100au));
        close_graph(f);destroy(f);
    }
}
static void test_optional_redundant_state_and_start_receipt_cutoff(struct fake *f)
{
    create(f);f->suppress_upstream_state=1;bind(f,1);start(f);
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&count_opcode(f,0x0400100bu)==2);
    assert(status(f).initial_peer_acks==1&&status(f).rd_frame_bytes==1024);
    assert((status(f).startup_flags&127)==127&&!(status(f).startup_flags&128));
    assert(!status(f).rd_state_events&&status(f).last_rd_state==UINT32_MAX);
    state_event(f,0,1);pump(f,1);
    assert(!status(f).rd_state_events&&!status(f).error);
    state_event(f,2,0);pump(f,1);
    assert(status(f).rd_state_events==1&&status(f).last_rd_state==2&&(status(f).startup_flags&128));
    close_graph(f);destroy(f);

    /* Old PREPARE already received before the normal START ACK is superseded
     * by that ACK. A later STOP remains authoritative and cannot be ignored. */
    create(f);f->suppress_upstream_state=1;bind(f,1);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    state_event(f,1,0);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,20);
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&!status(f).error);
    assert(status(f).rd_state_events==1&&status(f).last_rd_state==1);
    assert(count_opcode(f,0x0400100bu)==2&&!(status(f).startup_flags&128));
    state_event(f,0,0);pump(f,1);
    assert(status(f).need_graph_close&&status(f).rd_state_events==2&&status(f).last_rd_state==0);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));close_graph(f);destroy(f);

    /* Newly received non-START states revoke even before ACTIVE/media ACK. */
    const uint32_t states[]={0,1,3};
    for(unsigned i=0;i<sizeof states/sizeof(*states);i++) {
        create(f);f->suppress_upstream_state=f->suppress_media_format=1;bind(f,1);start(f);
        assert(status(f).phase==SHIZUKA_TRANSPORT_CONFIGURED&&count_opcode(f,0x0400100bu)==2);
        state_event(f,states[i],0);pump(f,1);
        assert(status(f).need_graph_close&&status(f).error&&status(f).rd_state_events==1);
        assert(status(f).last_rd_state==states[i]&&!f->transforms&&!count_opcode(f,0x0400100au));
        close_graph(f);destroy(f);
    }
}
static void two_returned_reads(struct fake *f)
{
    create(f);bind(f,1);start(f);
    rd_done(f,nth_opcode(f,0x0400100bu,1),0);pump(f,1);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1);
    assert(count_opcode(f,0x0400100bu)==2&&status(f).unresolved_buffers==2);
}
static void test_one_write_credit_and_parked_returns(struct fake *f)
{
    two_returned_reads(f);
    struct sent wrong=*nth_opcode(f,0x0400100au,0);wrong.packet.token++;
    wr_done(f,&wrong);pump(f,2);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1&&count_opcode(f,0x0400100bu)==2);
    assert(!status(f).wr_frames&&!status(f).error);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,1);
    assert(status(f).wr_frames==1&&f->transforms==2&&count_opcode(f,0x0400100au)==2);
    assert(read32(nth_opcode(f,0x0400100au,1)->payload+8)==102&&count_opcode(f,0x0400100bu)==3);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,2); /* duplicate cannot mint credit */
    assert(status(f).wr_frames==1&&f->transforms==2&&count_opcode(f,0x0400100au)==2);
    close_graph(f);assert(f->freed==2);destroy(f);

    /* Even a successful BASIC response is not a data lease return. */
    two_returned_reads(f);basic(f,&nth_opcode(f,0x0400100au,0)->packet,0);pump(f,1);
    assert(status(f).need_graph_close&&status(f).error&&!status(f).wr_frames);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1&&!f->freed);
    close_graph(f);assert(f->freed==2);destroy(f);

    /* Matching token but incorrect address never releases WR credit. */
    two_returned_reads(f);
    uint8_t bad[32]={0};const struct sent *w=nth_opcode(f,0x0400100au,0);
    write32(bad+8,read32(w->payload+8)+1);write32(bad+16,4096);write32(bad+24,read32(w->payload+36));
    deliver(f,SHIZUKA_TRANSPORT_WR_MIID,w->packet.token,0x05001004u,bad,sizeof bad);pump(f,1);
    assert(status(f).need_graph_close&&status(f).error&&!status(f).wr_frames);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1&&!f->freed);
    close_graph(f);assert(f->freed==2);destroy(f);

    two_returned_reads(f);f->now+=2001;pump(f,1);
    assert(status(f).need_graph_close&&status(f).phase==SHIZUKA_TRANSPORT_UNCERTAIN);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,2);
    assert(!status(f).wr_frames&&f->transforms==1&&count_opcode(f,0x0400100au)==1&&!f->freed);
    close_graph(f);assert(f->freed==2);destroy(f);

    /* STOP can discard the already returned RD while the single WR is owned
     * remotely. Only its exact WR_DONE then permits pause/resume progress. */
    two_returned_reads(f);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPED,0));pump(f,1);
    assert(f->discards==1&&f->transforms==1&&!status(f).pause_ready&&count_opcode(f,0x0400100au)==1);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,1);
    assert(status(f).pause_ready&&!status(f).wr_frames&&!status(f).need_graph_close);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,20);
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&count_opcode(f,0x0400100bu)==4);
    rd_done(f,nth_opcode(f,0x0400100bu,2),0);pump(f,1);
    assert(f->transforms==2&&count_opcode(f,0x0400100au)==2);
    close_graph(f);assert(f->freed==2);destroy(f);

    /* Normal CLOSE retires both WR and a parked RD without sending it. */
    two_returned_reads(f);close_graph(f);
    assert(f->freed==2&&f->transforms==1&&count_opcode(f,0x0400100au)==1);
    assert(count_opcode(f,0x0100100du)==2);destroy(f);

    /* Exercise continuing reuse beyond the three-frame actual stall. Every
     * next RD can return while WR is owned; order and one credit stay exact. */
    two_returned_reads(f);
    for(uint32_t i=0;i<32;i++){
        assert(count_opcode(f,0x0400100au)==i+1);
        const struct sent *write=nth_opcode(f,0x0400100au,i);
        assert(read32(write->payload+8)==101+(i&1));
        wr_done(f,write);pump(f,1);
        assert(status(f).wr_frames==i+1&&count_opcode(f,0x0400100au)==i+2);
        assert(count_opcode(f,0x0400100bu)==i+3);
        rd_done(f,nth_opcode(f,0x0400100bu,i+2),0);pump(f,1);
        assert(count_opcode(f,0x0400100au)==i+2&&f->transforms==i+2&&!status(f).error);
    }
    close_graph(f);assert(f->freed==2);destroy(f);
}
static void test_olc_wire_flags_do_not_invent_codec_eof(struct fake *f)
{
    create(f);bind(f,1);start(f);
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    const struct sent *primary=nth_opcode(f,0x0400100au,0);
    assert(read32(primary->payload+24)==0x80000000u&&read32(primary->payload+16)==42);
    assert(!read32(primary->payload+20));
    /* The C0 parser result still contains its legacy0x10 boundary marker.
     * Transport must not turn that into CAPI EOF or invent a timestamp. */
    rd_done_kind(f,nth_opcode(f,0x0400100bu,1),0,1);pump(f,1);
    assert(count_opcode(f,0x0400100au)==1&&f->transforms==1);
    wr_done(f,primary);pump(f,1);
    const struct sent *secondary=nth_opcode(f,0x0400100au,1);
    assert(!read32(secondary->payload+24)&&!read32(secondary->payload+16)&&!read32(secondary->payload+20));
    assert(read32(secondary->payload+40)==28&&status(f).rewritten_wrappers==1);
    wr_done(f,secondary);pump(f,1);
    assert(status(f).wr_frames==2&&status(f).rewritten_wrappers==1&&!status(f).error);
    rd_done(f,nth_opcode(f,0x0400100bu,2),0);pump(f,1);
    assert(read32(nth_opcode(f,0x0400100au,2)->payload+24)==0x80000000u);
    close_graph(f);destroy(f);

    /* Masking is performed only AFTER full result validation. A helper must
     * not silently accept an unreviewed continuation or reserved flag. */
    const uint32_t unknown[]={0x20000000u,0x40000000u,0x20u,0x1u};
    for(unsigned i=0;i<sizeof unknown/sizeof(*unknown);i++){
        create(f);bind(f,1);start(f);f->parser_extra_flags=unknown[i];
        rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
        assert(status(f).error&&status(f).need_graph_close&&!count_opcode(f,0x0400100au));
        close_graph(f);destroy(f);
    }
}
static void test_wr_state_observation(struct fake *f)
{
    create(f);assert(!status(f).wr_state_bytes&&!status(f).wr_state_events&&status(f).last_wr_state==UINT32_MAX);
    destroy(f);
    for(uint32_t value=0;value<=3;value++) {
        create(f);bind(f,1);pump(f,30);
        assert(!status(f).wr_state_bytes&&!status(f).wr_state_events&&status(f).last_wr_state==UINT32_MAX);
        uint32_t pairs[]={2,1,3,value};
        property_event(f,0x03001005u,pairs,2);pump(f,3);
        assert(status(f).wr_state_bytes==24&&status(f).wr_state_events==1&&status(f).last_wr_state==value);
        property_event(f,0x03001005u,pairs,2);pump(f,2);
        assert(status(f).wr_state_events==2&&status(f).last_wr_state==value);
        uint8_t b[16]={0};write32(b,SHIZUKA_TRANSPORT_WR_MIID);write32(b+4,1);write32(b+8,3);write32(b+12,(value+1)%4);
        deliver(f,SHIZUKA_TRANSPORT_WR_MIID,f->registration_tokens[7]+1,0x03001005u,b,16);pump(f,1);
        deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[7],0x03001005u,b,16);pump(f,1);
        assert(status(f).wr_state_events==2&&status(f).last_wr_state==value);
        uint32_t rt[]={2,0};property_event(f,0x03001005u,rt,1);pump(f,2);
        assert(status(f).wr_state_bytes==24&&status(f).wr_state_events==2&&status(f).last_wr_state==value);
        close_graph(f);assert(status(f).wr_state_events==2&&status(f).last_wr_state==value);
        bind_epoch(f,1,2);assert(!status(f).wr_state_bytes&&!status(f).wr_state_events&&status(f).last_wr_state==UINT32_MAX);
        assert(!shizuka_transport_graph_stage(f->context,2,SHIZUKA_GRAPH_CLOSED,0));pump(f,12);destroy(f);
    }
    /* Whole-event validation precedes diagnostics. A valid state followed by
     * a malformed/misdirected property does not publish a partial observation. */
    for(uint32_t bad=0;bad<3;bad++) {
        create(f);bind(f,1);pump(f,30);
        uint32_t pairs[]={3,2,bad==0?1:bad==1?4:3,bad==2?4:0};
        property_event(f,0x03001005u,pairs,2);pump(f,1);
        assert(status(f).need_graph_close&&!status(f).wr_state_bytes&&!status(f).wr_state_events&&status(f).last_wr_state==UINT32_MAX);
        close_graph(f);destroy(f);
    }
    /* State2 is an observation, never a substituted owner START. State1 after
     * owner START still revokes, and that exact fatal observation is retained. */
    create(f);bind(f,1);pump(f,30);uint32_t pairs[]={3,2};
    property_event(f,0x03001005u,pairs,1);pump(f,3);
    assert(status(f).last_wr_state==2&&!count_opcode(f,0x0400100bu)&&!f->transforms);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,10);
    pairs[1]=1;property_event(f,0x03001005u,pairs,1);pump(f,1);
    assert(status(f).need_graph_close&&status(f).wr_state_bytes==16&&status(f).wr_state_events==2&&status(f).last_wr_state==1);
    close_graph(f);destroy(f);
}
static void test_raw_prepared_read_barrier(struct fake *f)
{
    create(f);bind(f,1);pump(f,30);
    assert(shizuka_transport_prepare_reads(f->context,1)<0); /* No PREPARE ACK. */
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
#if !SHIZUKA_RELAY_RAW_ISLAND
    assert(shizuka_transport_prepare_reads(f->context,1)<0);
    assert(shizuka_transport_prepared_reads_ready(f->context,1)<0);
    pump(f,10);assert(!count_opcode(f,0x0400100bu));close_graph(f);destroy(f);
#else
    assert(shizuka_transport_prepare_reads(f->context,2)<0);
    assert(!shizuka_transport_prepare_reads(f->context,1));
    assert(!shizuka_transport_prepare_reads(f->context,1)); /* Idempotent. */
    assert(!shizuka_transport_prepared_reads_ready(f->context,1));
    pump(f,10);assert(shizuka_transport_prepared_reads_ready(f->context,1)==1);
    assert((status(f).startup_flags&0x7000u)==0x7000u);
    assert(!status(f).rd_state_events&&!status(f).rd_frames&&!status(f).wr_frames);
    assert(status(f).phase==SHIZUKA_TRANSPORT_CONFIGURED&&!f->transforms);
    assert(count_opcode(f,0x0400100bu)==2&&!count_opcode(f,0x0400100au));
    const struct sent *seed=nth_opcode(f,0x01001006u,0);
    assert(seed->packet.dst_port==SHIZUKA_TRANSPORT_RD_MIID&&read32(seed->payload+36)==3&&read32(seed->payload+40)==1);
    /* Real data can reach the queue after START-send but before START-ACK.
     * Hold it; the completed prequeue barrier remains valid for that ACK. */
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    assert(!f->transforms&&!count_opcode(f,0x0400100au));
    assert(!shizuka_transport_prepared_reads_ready(f->context,1));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,10);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1);
    seed=nth_opcode(f,0x01001006u,1);assert(read32(seed->payload+40)==2);
    close_graph(f);destroy(f);

    /* Genuine MF-on-first-READ ordering: no format at event registration.
     * Empty ownership return carries an invalid nonzero timestamp and is
     * recycled without transform/counter/WR, before owner START. */
    create(f);f->suppress_media_format=1;f->prestart_format_empty=1;bind(f,1);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_prepare_reads(f->context,1));pump(f,2);
    assert(!shizuka_transport_prepared_reads_ready(f->context,1)); /* Queued callback not drained. */
    pump(f,10);assert(shizuka_transport_prepared_reads_ready(f->context,1)==1);
    assert(status(f).media_format_bytes==12&&status(f).media_format_ready);
    assert(count_opcode(f,0x0400100bu)==3&&!status(f).rd_frames&&!status(f).wr_frames&&!f->transforms);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,10);
    rd_done(f,nth_opcode(f,0x0400100bu,1),0);pump(f,1);
    assert(f->transforms==1&&count_opcode(f,0x0400100au)==1);
    close_graph(f);destroy(f);

    /* Seed ACK and a genuine OPFS are independent prerequisites. */
    create(f);bind(f,1);pump(f,30);f->suppress_opcode=0x01001006u;
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_prepare_reads(f->context,1));pump(f,5);
    assert(!shizuka_transport_prepared_reads_ready(f->context,1)&&!count_opcode(f,0x0400100bu));
    seed=nth_opcode(f,0x01001006u,0);
    struct sent wrong=*seed;wrong.packet.token++;basic(f,&wrong.packet,0);pump(f,1);
    assert(!count_opcode(f,0x0400100bu));basic(f,&seed->packet,0);pump(f,1);
    assert(!count_opcode(f,0x0400100bu));
    uint8_t opfs[12]={0};write32(opfs,2008);write32(opfs+4,1);write32(opfs+8,SHIZUKA_TRANSPORT_RD_MIID);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,f->registration_tokens[2],0x03001004u,opfs,12);pump(f,1);
    assert(shizuka_transport_prepared_reads_ready(f->context,1)==1);
    f->suppress_opcode=0;close_graph(f);destroy(f);

    /* Missing INIT, premature START, bounded timeout, and close while queued
     * cannot turn preparation into data authority or release unknown leases. */
    create(f);bind(f,0);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(shizuka_transport_prepare_reads(f->context,1)<0);close_graph(f);destroy(f);
    create(f);bind(f,1);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_prepare_reads(f->context,1));
    assert(shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0)<0);
    assert(status(f).need_graph_close&&!count_opcode(f,0x0400100bu));close_graph(f);destroy(f);
    create(f);f->suppress_frame_size=1;bind(f,1);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_prepare_reads(f->context,1));pump(f,10);f->now+=1001;pump(f,1);
    assert(shizuka_transport_prepared_reads_ready(f->context,1)<0&&status(f).need_graph_close);
    assert(!count_opcode(f,0x0400100bu));close_graph(f);destroy(f);

    /* Empty return is not a general zero-data escape: exact addresses/status,
     * zero frame/flags, observed MF, and a two-return diagnostic budget apply. */
    for(uint32_t invalid=0;invalid<6;invalid++) {
        create(f);if(invalid==4)f->suppress_media_format=1;
        bind(f,1);pump(f,30);
        assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
        assert(!shizuka_transport_prepare_reads(f->context,1));pump(f,10);
        const struct sent *read=nth_opcode(f,0x0400100bu,0);
        uint8_t empty[56]={0};uint32_t handle=read32(read->payload+8);
        write32(empty+12,handle);write32(empty+40,4096);write32(empty+48,handle);
        if(invalid==0)write32(empty+12,handle+1);
        if(invalid==1)write32(empty,1);
        if(invalid==2)write32(empty+28,0x80000000u);
        if(invalid==3)write32(empty+32,1);
        if(invalid==5) {
            rd_empty_done(f,read);rd_empty_done(f,nth_opcode(f,0x0400100bu,1));pump(f,2);
            read=nth_opcode(f,0x0400100bu,2);handle=read32(read->payload+8);
            write32(empty+12,handle);write32(empty+48,handle);
        }
        deliver(f,SHIZUKA_TRANSPORT_RD_MIID,read->packet.token,0x05001005u,empty,56);pump(f,1);
        assert(status(f).need_graph_close&&!f->transforms&&!count_opcode(f,0x0400100au));
        close_graph(f);assert(f->freed==2);destroy(f);
    }
    /* Metadata-only is not silently dropped or passed to the encoded parser.
     * It stays owned while waiting for START, then strict validation rejects. */
    create(f);bind(f,1);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_prepare_reads(f->context,1));pump(f,10);
    const struct sent *only_md=nth_opcode(f,0x0400100bu,0);
    uint8_t md_done[56]={0};uint32_t md_handle=read32(only_md->payload+8);
    write32(md_done+12,md_handle);write32(md_done+40,4096);write32(md_done+48,md_handle);write32(md_done+52,40);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,only_md->packet.token^0x10000u,0x05001005u,md_done,56);pump(f,1);
    assert(shizuka_transport_prepared_reads_ready(f->context,1)==1);
    deliver(f,SHIZUKA_TRANSPORT_RD_MIID,only_md->packet.token,0x05001005u,md_done,56);pump(f,1);
    assert(!status(f).error&&!f->transforms&&!count_opcode(f,0x0400100au));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,10);
    assert(status(f).need_graph_close&&!f->transforms&&!count_opcode(f,0x0400100au));close_graph(f);destroy(f);

    create(f);bind(f,1);pump(f,30);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PREPARED,0));
    assert(!shizuka_transport_prepare_reads(f->context,1));pump(f,10);
    assert(status(f).unresolved_buffers==2&&!f->freed);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPED,0));
    assert(shizuka_transport_prepared_reads_ready(f->context,1)<0);
    rd_empty_done(f,nth_opcode(f,0x0400100bu,0));rd_empty_done(f,nth_opcode(f,0x0400100bu,1));pump(f,1);
    assert(status(f).pause_ready&&!f->transforms&&!count_opcode(f,0x0400100au));
    close_graph(f);destroy(f);
#endif
}
int main(void)
{
    struct fake *f=calloc(1,sizeof(*f));uint32_t before;assert(f);
    test_rd_bootstrap_before_media_format(f);
    test_optional_redundant_state_and_start_receipt_cutoff(f);
    test_directional_peer_protocol(f);
    test_one_write_credit_and_parked_returns(f);
    test_olc_wire_flags_do_not_invent_codec_eof(f);
    create(f);assert(!f->sent_count);assert(!shizuka_transport_detach(f->context));free(f->context);
    create(f);bind(f,0);start(f);assert(!count_opcode(f,0x0400100bu));
    assert(shizuka_transport_attest_profile(f->context,2,1,1)<0);
    assert(!shizuka_transport_attest_profile(f->context,1,1,1));pump(f,2);
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&count_opcode(f,0x0400100bu)==2);
    /* Second read arrives first; neither transformation nor WR may overtake. */
    rd_done(f,nth_opcode(f,0x0400100bu,1),0);pump(f,1);assert(!f->transforms&&!count_opcode(f,0x0400100au));
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);assert(f->transforms==1&&count_opcode(f,0x0400100au)==1);
    assert(read32(nth_opcode(f,0x0400100au,0)->payload+8)==101);
    before=count_opcode(f,0x0400100bu);pump(f,2);assert(count_opcode(f,0x0400100bu)==before);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,1);assert(count_opcode(f,0x0400100bu)==before+1);
    assert(f->transforms==2&&count_opcode(f,0x0400100au)==2);
    assert(read32(nth_opcode(f,0x0400100au,1)->payload+8)==102);
    assert(status(f).wr_frames==1&&status(f).rewritten_wrappers==1&&f->sync_start==2&&f->sync_end==4);
    close_graph(f);assert(status(f).phase==SHIZUKA_TRANSPORT_CLOSED&&f->freed==2);
    assert(count_opcode(f,0x0100100du)==2);destroy(f);

    create(f);bind(f,1);start(f);rd_done(f,nth_opcode(f,0x0400100bu,0),1);pump(f,1);
    assert(status(f).need_graph_close&&status(f).unresolved_buffers==2&&!f->transforms&&!f->freed);
    close_graph(f);assert(f->freed==2);destroy(f);

    create(f);bind(f,1);start(f);f->output_md_bytes=28;
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    assert(read32(nth_opcode(f,0x0400100au,0)->payload+28)==4096);
    assert(read32(nth_opcode(f,0x0400100au,0)->payload+36)==101);
    assert(read32(nth_opcode(f,0x0400100au,0)->payload+40)==28);
    wr_done(f,nth_opcode(f,0x0400100au,0));pump(f,1);assert(status(f).wr_frames==1);
    close_graph(f);destroy(f);

    /* A queued response observed after the deadline can establish only the
     * original cleanup handle. It cannot revive configuration/data. */
    create(f);bind(f,1);f->suppress_opcode=0x0100100cu;pump(f,2);
    { uint8_t b[4];write32(b,101);f->now+=1001;
      deliver(f,1,nth_opcode(f,0x0100100cu,0)->packet.token,0x02001001u,b,4); }
    pump(f,1);assert(status(f).need_graph_close&&!status(f).configuration_ready);
    assert(!count_opcode(f,0x0100100eu)&&!count_opcode(f,0x0400100bu));
    f->suppress_opcode=0;close_graph(f);assert(f->freed==2&&count_opcode(f,0x0100100du)==1);destroy(f);

    /* A current-epoch rejected/unknown INIT requests ordinary close. It must
     * not become a delta-zero pass-through for an unreviewed codec. */
    create(f);bind(f,0);pump(f,30);assert(shizuka_transport_attest_profile(f->context,1,2,1)<0);
    assert(status(f).need_graph_close);pump(f,5);assert(!count_opcode(f,0x0400100bu));
    close_graph(f);destroy(f);

    create(f);bind(f,1);start(f);f->transform_error=1;
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    assert(status(f).need_graph_close&&!count_opcode(f,0x0400100au));close_graph(f);destroy(f);

    /* Definitely-not-sent and outcome-unknown are both fatal, but never
     * cause a second MAP, a blind UNMAP or premature vendor free. */
    create(f);bind(f,1);f->fail_opcode=0x0100100cu;f->fail_send=1;pump(f,2);
    assert(count_opcode(f,0x0100100cu)==1&&!status(f).pending_control);
    close_graph(f);assert(f->freed==1&&!count_opcode(f,0x0100100du));destroy(f);

    create(f);bind(f,1);f->fail_opcode=0x0100100cu;f->fail_send=2;pump(f,2);
    assert(count_opcode(f,0x0100100cu)==1&&status(f).pending_control);close_graph(f);assert(!f->freed);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_DSP_GENERATION_RESET,13));pump(f,5);destroy(f);

    create(f);bind(f,1);start(f);
    { struct shizuka_transport_packet unknown={0};uint32_t i;
      unknown.src_domain=2;unknown.dst_domain=3;unknown.src_port=SHIZUKA_TRANSPORT_RD_MIID;
      unknown.dst_port=SHIZUKA_TRANSPORT_CLIENT_PORT;unknown.opcode=0x03001009u;unknown.token=1;
      for(i=0;i<64;i++)assert(!f->receive(f->receive_context,&unknown));
      assert(f->receive(f->receive_context,&unknown)<0);
    }
    pump(f,1);assert(status(f).received_overflow&&status(f).need_graph_close&&!f->freed);
    close_graph(f);destroy(f);

    create(f);bind(f,1);f->suppress_opcode=0x0100100cu;pump(f,5);f->now+=1001;pump(f,1);
    assert(status(f).need_graph_close&&status(f).pending_control&&count_opcode(f,0x0100100cu)==1);
    close_graph(f);assert(!f->freed&&status(f).phase!=SHIZUKA_TRANSPORT_CLOSED);
    pump(f,10);assert(count_opcode(f,0x0100100cu)==1&&!count_opcode(f,0x0100100du));
    assert(shizuka_transport_detach(f->context)<0);
    /* Only independently confirmed DSP generation reset discharges unknown MAP. */
    assert(shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_DSP_GENERATION_RESET,12)<0);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_DSP_GENERATION_RESET,13));pump(f,5);destroy(f);

    create(f);bind(f,1);start(f);f->unmap_error=1;close_graph(f);
    assert(count_opcode(f,0x0100100du)==1&&!f->freed&&status(f).phase==SHIZUKA_TRANSPORT_UNCERTAIN);
    pump(f,10);assert(count_opcode(f,0x0100100du)==1);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_DSP_GENERATION_RESET,13));pump(f,5);destroy(f);

    create(f);bind(f,1);start(f);f->now+=2001;pump(f,1);
    assert(status(f).need_graph_close&&status(f).unresolved_buffers==2&&!f->freed);
    close_graph(f);assert(f->freed==2);destroy(f);

    /* Ordinary STOP/START retains graph epoch, maps, event registrations and
     * fixed delta. STOP_ACK alone does not discharge any buffer lease. */
    create(f);bind(f,1);start(f);assert(shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPED,0)<0);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    assert(status(f).phase==SHIZUKA_TRANSPORT_PAUSING&&!status(f).need_graph_close);
    assert(shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0)<0);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPED,0));pump(f,2);
    assert(status(f).unresolved_buffers==2&&!status(f).pause_ready&&!f->freed);
    rd_empty_done(f,nth_opcode(f,0x0400100bu,0));rd_empty_done(f,nth_opcode(f,0x0400100bu,1));pump(f,1);
    assert(status(f).pause_ready&&status(f).phase==SHIZUKA_TRANSPORT_PAUSED&&!status(f).need_graph_close);
    assert(count_opcode(f,0x0100100cu)==2&&count_opcode(f,0x0100100eu)==8&&!count_opcode(f,0x0100100du));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0));pump(f,20);
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&status(f).delta_us==150000);
    assert(count_opcode(f,0x0400100bu)==4&&count_opcode(f,0x04001001u)==2&&f->allocated==2&&!f->freed);
    rd_done(f,nth_opcode(f,0x0400100bu,2),0);pump(f,1);assert(f->transforms==1);
    close_graph(f);destroy(f);

    /* Nonempty pause buffers need the strict discard validator; they are
     * neither transformed with the positive delta nor forwarded to WR. */
    create(f);bind(f,1);start(f);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STOPPED,0));
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_STARTED,0)); /* Fast resume waits for leases. */
    pump(f,1);assert(count_opcode(f,0x0400100bu)==2);
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);rd_empty_done(f,nth_opcode(f,0x0400100bu,1));pump(f,20);
    assert(f->discards==1&&!f->transforms&&!count_opcode(f,0x0400100au));
    assert(status(f).phase==SHIZUKA_TRANSPORT_ACTIVE&&count_opcode(f,0x0400100bu)==4);
    close_graph(f);destroy(f);

    create(f);bind(f,1);start(f);f->discard_error=1;
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    rd_done(f,nth_opcode(f,0x0400100bu,0),0);pump(f,1);
    assert(status(f).need_graph_close&&!count_opcode(f,0x0400100au));close_graph(f);destroy(f);

    create(f);bind(f,1);f->suppress_frame_size=1;start(f);
    assert(!count_opcode(f,0x0400100bu));f->now+=1001;pump(f,1);
    assert(status(f).need_graph_close&&!count_opcode(f,0x0400100bu));close_graph(f);destroy(f);

    create(f);bind(f,1);start(f);
    assert(!shizuka_transport_graph_stage(f->context,1,SHIZUKA_GRAPH_PAUSING,0));
    rd_empty_done(f,nth_opcode(f,0x0400100bu,0));rd_empty_done(f,nth_opcode(f,0x0400100bu,1));pump(f,1);
    f->now+=1001;pump(f,1);assert(status(f).need_graph_close&&!status(f).pause_ready);
    close_graph(f);destroy(f);
    test_media_format_observation(f);test_wr_peer_observation(f);test_wr_state_observation(f);test_raw_prepared_read_barrier(f);
    free(f);puts("HAL transport contract passed: existing registry, OPEN-before-INIT, early events, accepted media-format length, MAP/format/peer ACKs, ordered frames, cache transitions, no premature reuse, malformed DONE, timeout/no retry and close/reset cleanup");
    return 0;
}
