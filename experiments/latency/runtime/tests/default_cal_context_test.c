/* SPDX-License-Identifier: GPL-2.0-only */
#include "default_cal_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks,strip_calls;
#define check(v) do {++checks;if(!(v)){fprintf(stderr,"check %u failed at line %d\n",checks,__LINE__);abort();}} while(0)
static struct shizuka_graph_bridge b;
static struct shizuka_graph_bridge_env e;
static _Alignas(16) uint8_t frame[0x1f0],node[0xc0],vector[16];
static struct shizuka_gsl_key keys[SHIZUKA_GSL_MAX_KEYS];
static uint32_t ids[SHIZUKA_GSL_MAX_SGIDS];
static uintptr_t fp,node_arg;
static const void *command,*response;
static uint32_t pc,command_size,response_size;
static uintptr_t strip(uintptr_t value)
{++strip_calls;return value&UINT64_C(0x0000ffffffffffff);}
static void put32(void *p,uint32_t value)
{uint8_t *d=p;for(unsigned i=0;i<4;i++)d[i]=(uint8_t)(value>>(i*8));}
static void put64(void *p,uintptr_t value)
{put32(p,(uint32_t)value);put32((uint8_t *)p+4,(uint32_t)(value>>32));}
static void init(void)
{
    memset(&b,0,sizeof b);memset(&e,0,sizeof e);memset(frame,0,sizeof frame);
    memset(node,0,sizeof node);memset(vector,0,sizeof vector);
    memset(keys,0,sizeof keys);memset(ids,0,sizeof ids);
    e.scope=(struct shizuka_relay_scope){0x40000,0x12000,9,0x2012,0x535a6101,15,1,0,0};
    b.transaction.scope=e.scope;b.transaction.phase=SHIZUKA_RELAY_ACTIVE;
    b.transaction.num_sgids=3;b.transaction.num_keys=3;
    b.transaction.sgids[0]=0xb000006c;b.transaction.sgids[1]=SHIZUKA_RELAY_SG;
    b.transaction.sgids[2]=0xb0000099;
    b.transaction.keys[0]=(struct shizuka_gsl_key){0xa2000000,0xa2000003};
    b.transaction.keys[1]=(struct shizuka_gsl_key){0xb4000000,0xb4000002};
    b.transaction.keys[2]=(struct shizuka_gsl_key){0xb5000000,0xb5000003};
    memcpy(keys,b.transaction.keys,3*sizeof(*keys));memcpy(ids,b.transaction.sgids,3*sizeof(*ids));
    e.library_base=b.library_base=0x1000000;b.open_accepted=1;b.frozen_context_seq=10;
    fp=(uintptr_t)frame+0x50;command=(void *)(fp-0x40);response=(void *)(fp-0x50);
    node_arg=(uintptr_t)node;pc=0x1e1c0;command_size=36;response_size=12;
    uintptr_t parent=fp+0x190;
    put64((void *)fp,parent);
    put64((void *)(fp+8),UINT64_C(0xabc0000000000000)|(b.library_base+0x251f0));
    put64((void *)(parent+8),UINT64_C(0xdef0000000000000)|(b.library_base+0x23e80));
    put64((void *)(parent-0xe0),node_arg);put64((void *)(parent-0xd8),(uintptr_t)vector);
    put32(vector,3);put64(vector+8,(uintptr_t)keys);
    put32((void *)command,3);put64((uint8_t *)command+4,(uintptr_t)ids);
    strip_calls=0;
}
static int invoke(struct shizuka_default_cal_view *out)
{
    struct shizuka_graph_bridge before_b=b;struct shizuka_graph_bridge_env before_e=e;
    uint8_t before_frame[sizeof frame],before_node[sizeof node],before_vector[sizeof vector];
    struct shizuka_gsl_key before_keys[SHIZUKA_GSL_MAX_KEYS];
    uint32_t before_ids[SHIZUKA_GSL_MAX_SGIDS];
    memcpy(before_frame,frame,sizeof frame);memcpy(before_node,node,sizeof node);
    memcpy(before_vector,vector,sizeof vector);memcpy(before_keys,keys,sizeof keys);
    memcpy(before_ids,ids,sizeof ids);
    int result=shizuka_default_cal_context_view(&b,&e,pc,command,command_size,response,response_size,
                                               node_arg,fp,strip,out);
    check(!memcmp(&b,&before_b,sizeof b));check(!memcmp(&e,&before_e,sizeof e));
    check(!memcmp(frame,before_frame,sizeof frame));check(!memcmp(node,before_node,sizeof node));
    check(!memcmp(vector,before_vector,sizeof vector));check(!memcmp(keys,before_keys,sizeof keys));
    check(!memcmp(ids,before_ids,sizeof ids));
    return result;
}
static void reject(void)
{
    struct shizuka_default_cal_view out={(const void *)(uintptr_t)0x7770,77};
    check(!invoke(&out));check(out.keys==(const void *)(uintptr_t)0x7770 && out.num_keys==77);
}
int main(void)
{
    struct shizuka_default_cal_view out={0};
    init();check(invoke(&out));check(out.keys==keys && out.num_keys==3);
    check(out.keys!=b.transaction.keys);check(strip_calls==2);
    pc=0x1e2d8;check(invoke(&out)); /* same active frame for fill */
    /* SG order need not match; membership must be exact and unique. */
    init();uint32_t first=ids[0];ids[0]=ids[2];ids[2]=first;check(invoke(&out));
    /* Lease/context expiry cannot disable needed owned logger-cal filtering. */
    init();e.scope.flags&=~SHIZUKA_GSL_TRANSPORT_READY;e.provider.enabled=0;e.transport_ready=0;
    check(invoke(&out));
    for(unsigned variant=0;variant<14;variant++) {
        init();
        switch(variant) {
        case 0:pc=0x246e4;break;
        case 1:command_size=35;break;
        case 2:response_size=13;break;
        case 3:b.open_accepted=0;break;
        case 4:b.transaction.phase=SHIZUKA_RELAY_FILLED;break;
        case 5:b.transaction.phase=SHIZUKA_RELAY_FAILED;break;
        case 6:b.transaction.profile_confirmed=1;break;
        case 7:b.frozen_context_seq=0;break;
        case 8:b.frozen_context_seq=11;break;
        case 9:e.library_base=0;break;
        case 10:e.library_base++;break;
        case 11:e.library_base=b.library_base=UINTPTR_MAX-0x1000;break;
        case 12:command=(uint8_t *)command+1;break;
        case 13:response=(uint8_t *)response+1;break;
        }
        reject();check(!strip_calls);
    }
    for(unsigned variant=0;variant<13;variant++) {
        init();
        switch(variant) {
        case 0:e.scope.graph++;break;
        case 1:e.scope.thread++;break;
        case 2:e.scope.epoch++;break;
        case 3:e.scope.source_port++;break;
        case 4:e.scope.client_port++;break;
        case 5:e.scope.flags&=~SHIZUKA_GSL_SOURCE_REGISTERED;break;
        case 6:b.transaction.scope.flags&=~SHIZUKA_GSL_SOURCE_REGISTERED;break;
        case 7:e.scope.open_refs=2;break;
        case 8:e.scope.start_refs=1;break;
        case 9:e.scope.stop_refs=1;break;
        case 10:b.transaction.scope.open_refs=0;break;
        case 11:e.scope.source_port=b.transaction.scope.source_port=0;break;
        case 12:e.scope.source_port=b.transaction.scope.source_port=UINT32_MAX;break;
        }
        reject();check(!strip_calls);
    }
    for(unsigned variant=0;variant<11;variant++) {
        init();uintptr_t parent=fp+0x190;
        switch(variant) {
        case 0:put64((void *)fp,parent+16);break;
        case 1:put64((void *)(fp+8),e.library_base+0x1df50);break; /* explicit set_cal */
        case 2:put64((void *)(parent+8),e.library_base+0x27158);break; /* graph change */
        case 3:put64((void *)(fp+8),e.library_base+0x251f4);break;
        case 4:put64((void *)(parent-0xe0),node_arg+8);break;
        case 5:put32(node+0x18,1);break;
        case 6:put64(node+0x20,(uintptr_t)keys);break;
        case 7:node_arg=0;break;
        case 8:node_arg++;break;
        case 9:node_arg=UINTPTR_MAX-7;break;
        case 10:put64((void *)(parent-0xd8),0);break;
        }
        reject();
    }
    for(unsigned variant=0;variant<9;variant++) {
        init();
        switch(variant) {
        case 0:put32((void *)command,0);break;
        case 1:put32((void *)command,2);break;
        case 2:put32((void *)command,SHIZUKA_GSL_MAX_SGIDS+1);break;
        case 3:put64((uint8_t *)command+4,0);break;
        case 4:put64((uint8_t *)command+4,(uintptr_t)ids+1);break;
        case 5:ids[0]=ids[1];break;
        case 6:ids[2]=0xb0000098;break;
        case 7:ids[0]=0;break;
        case 8:b.transaction.sgids[2]=b.transaction.sgids[0];break;
        }
        reject();
    }
    for(unsigned variant=0;variant<12;variant++) {
        init();
        switch(variant) {
        case 0:put32(vector,0);break;
        case 1:put32(vector,2);break;
        case 2:put32(vector,SHIZUKA_GSL_MAX_KEYS+1);break;
        case 3:put64(vector+8,0);break;
        case 4:put64(vector+8,(uintptr_t)keys+1);break;
        case 5:put64(vector+8,UINTPTR_MAX-3);break;
        case 6:keys[1].key++;break;
        case 7:keys[2].value++;break;
        case 8:{struct shizuka_gsl_key x=keys[1];keys[1]=keys[2];keys[2]=x;break;}
        case 9:keys[2]=keys[0];b.transaction.keys[2]=keys[0];break;
        case 10:put64((void *)(fp+0x190-0xd8),(uintptr_t)vector+1);break;
        case 11:put64((void *)(fp+0x190-0xd8),UINTPTR_MAX-7);break;
        }
        reject();
    }
    /* Upper-bound vectors and a late SG duplicate exercise all loops. */
    init();b.transaction.num_sgids=SHIZUKA_GSL_MAX_SGIDS;
    for(unsigned i=0;i<SHIZUKA_GSL_MAX_SGIDS;i++)ids[i]=b.transaction.sgids[i]=0xb1000000+i;
    ids[127]=b.transaction.sgids[127]=SHIZUKA_RELAY_SG;put32((void *)command,SHIZUKA_GSL_MAX_SGIDS);
    b.transaction.num_keys=SHIZUKA_GSL_MAX_KEYS;put32(vector,SHIZUKA_GSL_MAX_KEYS);
    for(unsigned i=0;i<SHIZUKA_GSL_MAX_KEYS;i++)keys[i]=b.transaction.keys[i]=(struct shizuka_gsl_key){0xa0000000+i,i};
    check(invoke(&out));check(out.keys==keys && out.num_keys==64);
    ids[127]=ids[0];reject();
    /* Invalid integer ranges fail without a speculative frame read. */
    init();fp=0x40;command=NULL;response=NULL;reject();
    init();fp=UINTPTR_MAX-15;command=(void *)(fp-0x40);response=(void *)(fp-0x50);reject();
    init();fp++;command=(void *)(fp-0x40);response=(void *)(fp-0x50);reject();
    init();
    check(!shizuka_default_cal_context_view(NULL,&e,pc,command,36,response,12,node_arg,fp,strip,&out));
    check(!shizuka_default_cal_context_view(&b,NULL,pc,command,36,response,12,node_arg,fp,strip,&out));
    check(!shizuka_default_cal_context_view(&b,&e,pc,command,36,response,12,node_arg,fp,NULL,&out));
    check(!shizuka_default_cal_context_view(&b,&e,pc,command,36,response,12,node_arg,fp,strip,NULL));
    printf("default_cal_context: %u checks; failures=0\n",checks);
    return 0;
}
