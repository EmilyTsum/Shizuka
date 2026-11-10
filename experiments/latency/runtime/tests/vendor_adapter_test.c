/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200112L
#include "shizuka/vendor_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_memory { void *address; uint64_t *metadata; uint8_t *sys_id; uint32_t fd, cpu_owned; };
struct fake {
    void *adapter, *registered_context;
    shizuka_vendor_gpr_callback registered_callback;
    struct shizuka_transport_host host;
    uint32_t register_calls, deregister_calls, alloc_calls, send_calls, packet_frees;
    uint32_t delivered, faults, last_fault, reject_receive, deregister_in_receive;
    uint32_t alloc_error, alloc_error_with_packet, alloc_bad_header, send_error, free_error;
    uint32_t shmem_calls, shmem_frees, ioctl_calls, ioctl_fail, shmem_fail, partial_failure;
    uint32_t transform_calls, transform_status, before_ttp, after_ttp;
    uint32_t notified_phase, notified_close;
    struct shizuka_vendor_gpr_packet *packets[64];
    struct shizuka_vendor_gpr_packet *nested;
    struct fake_memory memory[2];
};
static struct fake *F;
static uint32_t get32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t value) { unsigned i;for(i=0;i<4;i++)p[i]=(uint8_t)(value>>(8*i)); }
static struct shizuka_vendor_gpr_packet *new_packet(uint32_t bytes)
{
    unsigned i;struct shizuka_vendor_gpr_packet *p=calloc(1,24+bytes);assert(p);
    p->header=((24+bytes)<<8)|0x60u;
    for(i=0;i<64;i++)if(!F->packets[i]){F->packets[i]=p;return p;}assert(0);return 0;
}
static uint32_t packet_free(struct shizuka_vendor_gpr_packet *packet)
{
    unsigned i;for(i=0;i<64;i++)if(F->packets[i]==packet){
        F->packets[i]=0;free(packet);F->packet_frees++;return F->free_error;
    }assert(!"packet freed twice or not owned");return 1;
}
static uint32_t root_callback(struct shizuka_vendor_gpr_packet *p,void *context)
{ return shizuka_vendor_receive(context,p); }
static uint32_t gpr_register(uint32_t port,shizuka_vendor_gpr_callback callback,void *context)
{
    assert(port==SHIZUKA_TRANSPORT_CLIENT_PORT&&callback==root_callback&&context==F->adapter);
    F->register_calls++;F->registered_callback=callback;F->registered_context=context;return 0;
}
static uint32_t gpr_deregister(uint32_t port)
{assert(port==SHIZUKA_TRANSPORT_CLIENT_PORT);F->deregister_calls++;return 0;}
static uint32_t gpr_alloc(struct shizuka_vendor_gpr_alloc *a)
{
    struct shizuka_vendor_gpr_packet *p;
    F->alloc_calls++;assert(a->src_domain==3&&a->src_port==SHIZUKA_TRANSPORT_CLIENT_PORT);
    assert(a->dst_domain==2&&a->ret_packet&&a->payload_size<=1024);
    if(F->alloc_error&&!F->alloc_error_with_packet)return 1;
    p=new_packet(a->payload_size);p->src_domain=a->src_domain;p->dst_domain=a->dst_domain;p->client_data=a->client_data;
    p->src_port=a->src_port;p->dst_port=a->dst_port;p->token=a->token;p->opcode=a->opcode;
    if(F->alloc_bad_header)p->header^=0x10;
    *a->ret_packet=p;return F->alloc_error;
}
static uint32_t gpr_send(struct shizuka_vendor_gpr_packet *packet)
{
    F->send_calls++;assert(packet->src_port==SHIZUKA_TRANSPORT_CLIENT_PORT);
    if(F->send_error)return 1;
    /* Accepted packet is consumed by fake GPR, not adapter. */
    return packet_free(packet);
}
static int32_t shmem_alloc(struct shizuka_vendor_shmem *info)
{
    unsigned i;void *address=0;struct fake_memory *m=0;
    F->shmem_calls++;assert(!info->cache_type&&info->buf_size==16384);
    assert(!info->mem_type&&!info->index_type&&!info->vaddr&&!info->metadata);
    assert(info->num_sys_id==1&&info->sys_id&&*info->sys_id==2&&!info->platform_info&&!info->flags);
    if(F->shmem_fail&&!F->partial_failure)return 1;
    for(i=0;i<2;i++)if(!F->memory[i].address){m=&F->memory[i];break;}assert(m);
    assert(!posix_memalign(&address,64,16384));memset(address,0,16384);m->address=address;
    m->metadata=calloc(1,8);assert(m->metadata);m->fd=10+i;m->sys_id=info->sys_id;m->cpu_owned=1;
    info->vaddr=address;info->metadata=(uint64_t)(uintptr_t)m->metadata;
    info->mem_type=0;info->index_type=1;info->ipa_lsw=m->fd;
    return F->shmem_fail;
}
static int32_t shmem_free(struct shizuka_vendor_shmem *info)
{
    unsigned i;for(i=0;i<2;i++)if(F->memory[i].address==info->vaddr){
        struct fake_memory *m=&F->memory[i];assert(m->cpu_owned&&info->ipa_lsw==m->fd);
        assert(info->metadata==(uint64_t)(uintptr_t)m->metadata&&info->num_sys_id==1&&info->sys_id==m->sys_id&&*info->sys_id==2);
        /* Vendor free performs its own paired CPU END and FD teardown. */
        free(m->address);free(m->metadata);memset(m,0,sizeof(*m));
        info->vaddr=0;info->metadata=0;F->shmem_frees++;return 0;
    }assert(!"foreign or duplicate memory free");return 1;
}
static long raw_ioctl(void *user,int32_t fd,uint32_t command,void *argument)
{
    unsigned i;uint64_t flags=*(uint64_t*)argument;assert(user==F&&command==0x40086200u);
    assert(flags==3||flags==7);F->ioctl_calls++;if(F->ioctl_fail)return -1;
    for(i=0;i<2;i++)if(F->memory[i].address&&F->memory[i].fd==(uint32_t)fd){
        assert(F->memory[i].cpu_owned==(flags==7));F->memory[i].cpu_owned=flags==3;return 0;
    }assert(!"sync of foreign FD");return -1;
}
static uint64_t now_ms(void *user){assert(user==F);return 123456789ULL;}
static void notify(void *user,uint32_t phase,int32_t error,uint32_t close)
{assert(user==F&&error==-4);F->notified_phase=phase;F->notified_close=close;}
static void receive_error(void *user,uint32_t reason)
{assert(user==F);F->faults++;F->last_fault=reason;}
static void transformed(void *user,int32_t status,uint32_t profile,uint64_t delta,const struct shz_encoded_result *result)
{
    assert(user==F&&profile==1&&delta==150000);F->transform_calls++;F->transform_status=(uint32_t)status;
    if(!status){assert(result);F->before_ttp=result->first_ttp_before_us;F->after_ttp=result->first_ttp_after_us;}
    else assert(!result);
}
static int32_t logical_receive(void *user,const struct shizuka_transport_packet *packet)
{
    assert(user==F&&packet->src_domain==2&&packet->dst_domain==3);
    assert(packet->src_port==1&&packet->dst_port==SHIZUKA_TRANSPORT_CLIENT_PORT);
    assert(packet->token==77&&packet->opcode==0x02001007&&packet->payload_bytes==4&&get32(packet->payload)==1);
    F->delivered++;
    if(F->nested){struct shizuka_vendor_gpr_packet *p=F->nested;F->nested=0;assert(!root_callback(p,F->adapter));}
    if(F->deregister_in_receive)assert(F->host.unregister_port(F->host.user,SHIZUKA_TRANSPORT_CLIENT_PORT)!=0);
    return F->reject_receive ? -1 : 0;
}
static struct shizuka_vendor_gpr_packet *incoming(void)
{
    struct shizuka_vendor_gpr_packet *p=new_packet(4);p->src_domain=2;p->dst_domain=3;p->src_port=1;
    p->dst_port=SHIZUKA_TRANSPORT_CLIENT_PORT;p->token=77;p->opcode=0x02001007;put32((uint8_t*)p+24,1);return p;
}
static struct shizuka_vendor_status status(void)
{
    struct shizuka_vendor_status s={0};s.abi=1;s.bytes=sizeof(s);assert(!shizuka_vendor_get_status(F->adapter,&s));return s;
}
static void create(struct fake *f)
{
    struct shizuka_vendor_functions functions={0};memset(f,0,sizeof(*f));F=f;
    f->adapter=calloc(1,shizuka_vendor_adapter_bytes());assert(f->adapter);
    functions.abi=1;functions.bytes=sizeof(functions);functions.abi_profile=SHIZUKA_VENDOR_DZIF_LAYOUT_V1;functions.identity_verified=1;
    functions.user=f;functions.gpr_register=gpr_register;functions.gpr_deregister=gpr_deregister;
    functions.gpr_alloc=gpr_alloc;functions.gpr_send=gpr_send;functions.gpr_free=packet_free;
    functions.shmem_alloc=shmem_alloc;functions.shmem_free=shmem_free;functions.callback_entry=root_callback;
    functions.raw_ioctl=raw_ioctl;functions.clock_ms=now_ms;functions.notify=notify;
    functions.receive_error=receive_error;functions.transform_result=transformed;
    assert(!shizuka_vendor_adapter_init(f->adapter,shizuka_vendor_adapter_bytes(),&functions));
    assert(shizuka_vendor_adapter_init(f->adapter,shizuka_vendor_adapter_bytes(),&functions)<0);
    assert(!shizuka_vendor_make_host(f->adapter,&f->host));assert(f->host.user==f->adapter);
    assert(!f->host.register_port(f->host.user,SHIZUKA_TRANSPORT_CLIENT_PORT,logical_receive,f));
}
static void dispose(struct fake *f)
{
    unsigned i;
    /* Synthetic test-process teardown of intentionally quarantined fake RAM.
     * It is not an adapter API or a valid real DSP/FD cleanup mechanism. */
    for(i=0;i<64;i++)if(f->packets[i])free(f->packets[i]);
    for(i=0;i<2;i++)if(f->memory[i].address){free(f->memory[i].address);free(f->memory[i].metadata);}
    free(f->adapter);F=0;
}
static struct shizuka_transport_packet ready_request(void)
{
    struct shizuka_transport_packet p={0};p.src_domain=3;p.dst_domain=2;
    p.src_port=SHIZUKA_TRANSPORT_CLIENT_PORT;p.dst_port=1;p.token=99;p.opcode=0x01001021u;return p;
}
static void empty_memory(struct shizuka_transport_memory *m){memset(m,0,sizeof(*m));m->fd=-1;m->subsystem_id=2;}
int main(void)
{
    struct fake f;struct shizuka_transport_packet request;struct shizuka_transport_memory memory[3];
    struct shizuka_vendor_gpr_packet *packet;unsigned i;
    create(&f);assert(!root_callback(incoming(),f.adapter));
    assert(f.delivered==1&&f.packet_frees==1&&status().packets_delivered==1&&!status().inflight);
    assert(f.host.clock_ms(f.host.user)==123456789);f.host.notify(f.host.user,5,-4,1);assert(f.notified_phase==5&&f.notified_close);
    for(i=0;i<8;i++){
        packet=incoming();switch(i){case 0:packet->header^=1;break;case 1:packet->header^=0x10;break;
        case 2:packet->header=(23u<<8)|0x60;break;case 3:packet->header=(2048u<<8)|0x60;break;
        case 4:packet->src_domain=3;break;case 5:packet->dst_port++;break;case 6:packet->reserved=1;break;default:packet->client_data=0xf0;}
        assert(!root_callback(packet,f.adapter));
    }
    assert(f.delivered==1&&f.packet_frees==9&&f.faults==8);
    f.reject_receive=1;assert(!root_callback(incoming(),f.adapter));assert(f.last_fault==SHIZUKA_VENDOR_RECEIVER_REJECTED);f.reject_receive=0;
    f.nested=incoming();assert(!root_callback(incoming(),f.adapter));assert(f.last_fault==SHIZUKA_VENDOR_CONCURRENT_CALLBACK);
    f.free_error=1;assert(!root_callback(incoming(),f.adapter));assert(f.last_fault==SHIZUKA_VENDOR_PACKET_FREE_FAILED);f.free_error=0;
    assert(status().packets_free_failed==1&&status().packets_received==13);
    f.deregister_in_receive=1;assert(!root_callback(incoming(),f.adapter));assert(f.deregister_calls==1&&!status().inflight);
    assert(!f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT));assert(f.deregister_calls==1&&status().retired);
    i=f.delivered;assert(!root_callback(incoming(),f.adapter));assert(f.delivered==i);
    assert(f.host.register_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT,logical_receive,&f)<0);dispose(&f);

    create(&f);request=ready_request();assert(!f.host.send(f.host.user,&request));assert(f.alloc_calls==1&&f.send_calls==1&&f.packet_frees==1);
    request.opcode=0x01001000;assert(f.host.send(f.host.user,&request)==1&&f.alloc_calls==1);request=ready_request();
    f.alloc_error=1;assert(f.host.send(f.host.user,&request)==1&&f.packet_frees==1);
    f.alloc_error_with_packet=1;assert(f.host.send(f.host.user,&request)==1&&f.packet_frees==2);
    f.alloc_error=f.alloc_error_with_packet=0;f.alloc_bad_header=1;assert(f.host.send(f.host.user,&request)==1&&f.packet_frees==3);
    f.alloc_bad_header=0;f.send_error=1;assert(f.host.send(f.host.user,&request)==2&&f.packet_frees==4);
    assert(!f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT));dispose(&f);

    create(&f);for(i=0;i<3;i++)empty_memory(&memory[i]);
    assert(!f.host.allocate(f.host.user,&memory[0]));assert(!f.host.allocate(f.host.user,&memory[1]));
    assert(f.host.allocate(f.host.user,&memory[2])<0&&f.shmem_calls==2);
    {struct shizuka_vendor_shmem info;memcpy(&info,memory[0].allocator_cookie,80);
     assert(info.sys_id==&memory[0].subsystem_id&&info.num_sys_id==1&&info.buf_size==16384);}
    assert(f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT)<0&&!f.deregister_calls);
    {uint8_t body[20]={3,0,1,0,4};request=ready_request();request.opcode=0x0100100c;request.client_data=1;request.payload=body;request.payload_bytes=20;
     put32(body+8,1234);put32(body+16,16384);assert(f.host.send(f.host.user,&request)==1);
     put32(body+8,(uint32_t)memory[0].fd);f.alloc_error=1;
     assert(f.host.send(f.host.user,&request)==1&&f.alloc_calls==1&&!f.send_calls);f.alloc_error=0;}
    assert(!f.host.cpu_sync(f.host.user,&memory[0],0));assert(f.ioctl_calls==1);
    assert(f.host.cpu_sync(f.host.user,&memory[0],0)<0&&f.ioctl_calls==1);
    assert(f.host.free(f.host.user,&memory[0])<0&&!f.shmem_frees);
    assert(!f.host.cpu_sync(f.host.user,&memory[0],1));assert(!f.host.free(f.host.user,&memory[0]));
    assert(!f.host.free(f.host.user,&memory[1]));assert(f.shmem_frees==2&&!status().allocations_owned);
    assert(f.host.free(f.host.user,&memory[0])<0);assert(!f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT));dispose(&f);

    create(&f);empty_memory(&memory[0]);assert(!f.host.allocate(f.host.user,&memory[0]));f.ioctl_fail=1;
    assert(f.host.cpu_sync(f.host.user,&memory[0],0)<0&&status().allocations_quarantined==1);
    assert(f.host.free(f.host.user,&memory[0])<0&&!f.shmem_frees);assert(f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT)<0);dispose(&f);
    create(&f);empty_memory(&memory[0]);f.shmem_fail=1;assert(f.host.allocate(f.host.user,&memory[0])<0);
    assert(!status().allocations_owned&&!f.shmem_frees);assert(!f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT));dispose(&f);
    create(&f);empty_memory(&memory[0]);f.shmem_fail=f.partial_failure=1;assert(f.host.allocate(f.host.user,&memory[0])<0);
    assert(status().allocations_quarantined==1&&!f.shmem_frees);assert(f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT)<0);dispose(&f);

    create(&f);{
        uint8_t data[356]={0},md[40]={0},original_md[40];struct shizuka_transport_transform_result out;
        data[0]=0x31;data[1]=0x60;data[2]=120;data[4]=0xa0;data[7]=0xae;
        put32(md,0x0a001052);put32(md+20,16);put32(md+24,356);put32(md+28,42);put32(md+36,0x80000000u);
        memcpy(original_md,md,40);
        assert(!f.host.validate_discard(f.host.user,1,data,356,356,md,40,40));
        assert(data[0]==0x31&&data[1]==0x60&&!f.transform_calls);memcpy(md,original_md,40);
        assert(!f.host.transform(f.host.user,1,150000,data,356,356,md,40,40,&out));
        assert(out.data_bytes==356&&!out.metadata_bytes&&out.timestamp_bits==42&&out.wrappers_changed==1);
        assert(f.transform_calls==1&&f.before_ttp==197000&&f.after_ttp==347000);
        data[0]=data[1]=0;assert(f.host.transform(f.host.user,1,150000,data,356,356,md,40,40,&out)!=0);
        assert(f.transform_calls==2&&f.transform_status);
    }assert(!f.host.unregister_port(f.host.user,SHIZUKA_TRANSPORT_CLIENT_PORT));dispose(&f);
    puts("vendor adapter contract passed: native ABIs, exact callback entry, normalization/free-once, in-flight/late callback retirement, send ownership, private OSAL/FD/cache lifetime and real encoded transform provenance");
    return 0;
}
