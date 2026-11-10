/* SPDX-License-Identifier: GPL-2.0-only */
#include "graph_bridge.h"
#include "graph_bridge_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct shizuka_graph_bridge b;
static struct shizuka_graph_bridge_env e;
static uint32_t ids[]={0xb000006c,0xb00000ab};
static struct shizuka_gsl_key keys[]={{0xa2000000,0xa2000003},{0xb4000000,0xb4000002},{0xb5000000,0xb5000003}};
static unsigned char cmdb[37],rspb[29],blob[sizeof(stock)+56],work[sizeof(stock)+56],calbuf[128];
#define cmd (cmdb+1)
#define rsp (rspb+1)
static void put32(void *p,uint32_t v){unsigned char *d=p;for(int i=0;i<4;i++)d[i]=(unsigned char)(v>>(8*i));}
static void put64(void *p,uintptr_t v){put32(p,(uint32_t)v);put32((char*)p+4,(uint32_t)((uint64_t)v>>32));}
static uint32_t get32(const void *p){const unsigned char *d=p;return d[0]|(uint32_t)d[1]<<8|(uint32_t)d[2]<<16|(uint32_t)d[3]<<24;}
static void init(void){
 memset(&b,0,sizeof b);memset(&e,0,sizeof e);memset(cmdb,0,sizeof cmdb);memset(rspb,0,sizeof rspb);
 e.scope=(struct shizuka_relay_scope){0x40000,0x12000,1,0x2012,0x535a6101,15,1,0,0};
 e.provider=(struct shizuka_relay_provider){7,1,150000};e.library_base=0x1000000;e.transport_ready=1;
 e.context=(S25LatencyContext){S25_LATENCY_CONTEXT_MAGIC,1,64,2,1,44100,4096,2,0x1234,{0}};
 e.current_gkv=keys;e.num_gkv=3;
 put32(cmd,2);put64(cmd+4,(uintptr_t)ids);put32(cmd+12,3);put64(cmd+16,(uintptr_t)keys);
 put32(rsp+16,sizeof stock);
}
static int after(uint32_t op,unsigned csize,unsigned rsize,int status,uintptr_t graph,uint32_t pc,struct shizuka_graph_bridge_result *r){
 return shizuka_graph_bridge_after_acdb(&b,&e,op,cmd,csize,rsp,rsize,status,graph,pc,work,sizeof work,r);
}
static void grow(void){struct shizuka_graph_bridge_result r;
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0);
 assert(r.action==SHIZUKA_RELAY_ADVERTISE && get32(rsp+16)==sizeof blob && b.frozen_context_seq==2);
 put32(rsp+16,sizeof stock);
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x247d0,&r)==0 && r.action==SHIZUKA_RELAY_ADVERTISE);
 memcpy(blob,stock,sizeof stock);put32(rsp+16,sizeof stock);put64(rsp+20,(uintptr_t)blob);e.spf_capacity=sizeof blob;
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x24e20,&r)==0 && r.action==SHIZUKA_RELAY_COMMITTED);
 assert(get32(rsp+16)==sizeof expanded && !memcmp(blob,expanded,sizeof blob));
}
static void calsetup(const unsigned char *data,unsigned n){
 memset(cmdb,0,sizeof cmdb);put32(cmd,2);put64(cmd+4,(uintptr_t)ids);
 memset(rspb,0,sizeof rspb);put32(rsp,n);
 struct shizuka_graph_bridge_result r;
 assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,0,e.scope.graph,0x1e1c0,&r)==0);
 assert(r.action==0 && get32(rsp)==n && b.calibration.pending);
 memcpy(calbuf,data,n);put64(rsp+4,(uintptr_t)calbuf);e.cal_capacity=n;
}
static void unrelated_calibration(void){
 static uint32_t other_ids[]={0xb000006c,0xb0000099};
 static uint32_t repeated_other_ids[]={0xb000006c,0xb000006c};
 static struct shizuka_gsl_key other_keys[]={{0xa3000000,0xdeadbeef}};
 static struct shizuka_gsl_key other_ckv[]={{0xb5000000,0xb5000001}};
 unsigned char owned_cmd[sizeof cmdb],owned_rsp[sizeof rspb],owned_data[sizeof calbuf];
 unsigned char call_cmd[sizeof cmdb],call_rsp[sizeof rspb];
 struct shizuka_graph_bridge_result r;
 /* Unrelated default/different-key calibration must not consume the pending
  * target query, activate an unconfirmed profile or revive a revoked source. */
 for(unsigned state=0;state<3;state++){
  init();grow();
  assert(shizuka_graph_bridge_open_ack(&b,&e,SHIZUKA_GRAPH_OPEN_OPCODE,e.scope.source_port,0));
  if(state){assert(shizuka_graph_bridge_confirm_profile(&b,&e,1,0x07001082,4096,44100,1));}
  if(state==2){
   e.scope.flags=7;
   assert(!shizuka_graph_bridge_confirm_profile(&b,&e,1,0x07001082,4096,44100,1));
   e.scope.flags=15;
   assert(b.transaction.phase==SHIZUKA_RELAY_FAILED);
  }
  uint32_t delta=state==1?150000:0;
  calsetup(cal,sizeof cal);
  memcpy(owned_cmd,cmdb,sizeof cmdb);memcpy(owned_rsp,rspb,sizeof rspb);
  memcpy(owned_data,calbuf,sizeof calbuf);
  struct shizuka_graph_bridge before=b;
  for(unsigned variant=0;variant<4;variant++){
   unsigned different=variant&1;
   e.current_gkv=different?other_keys:keys;e.num_gkv=different?1:3;
   for(unsigned attempt=0;attempt<4;attempt++){
    unsigned fill=attempt&1;int status=attempt>=2?7:0;
    memset(cmdb,0,sizeof cmdb);put32(cmd,2);
    /* A bounded non-target SG vector is untouched even if the original
     * ACDB validator would reject its repeated unrelated ID. */
    put64(cmd+4,(uintptr_t)(variant&2?repeated_other_ids:other_ids));
    if(different){
     put32(cmd+12,1);put64(cmd+16,(uintptr_t)other_ckv);
     put32(cmd+24,1);put64(cmd+28,(uintptr_t)other_ckv);
    }
    memset(rspb,0,sizeof rspb);put32(rsp,sizeof cal);
    memcpy(calbuf,cal,sizeof cal);
    if(fill)put64(rsp+4,(uintptr_t)calbuf);
    e.cal_capacity=sizeof cal;
    memcpy(call_cmd,cmdb,sizeof cmdb);memcpy(call_rsp,rspb,sizeof rspb);
    assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,status,e.scope.graph,fill?0x1e2d8:0x1e1c0,&r)==status);
    assert(r.action==SHIZUKA_RELAY_BYPASS && !r.removed);
    assert(!memcmp(call_cmd,cmdb,sizeof cmdb) && !memcmp(call_rsp,rspb,sizeof rspb));
    assert(!memcmp(calbuf,cal,sizeof cal) && !memcmp(&before,&b,sizeof b));
    assert(shizuka_graph_bridge_active_delta(&b,&e)==delta);
   }
  }
  e.current_gkv=keys;e.num_gkv=3;
  memcpy(cmdb,owned_cmd,sizeof cmdb);memcpy(rspb,owned_rsp,sizeof rspb);
  memcpy(calbuf,owned_data,sizeof calbuf);e.cal_capacity=sizeof cal;
  assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,0,e.scope.graph,0x1e2d8,&r)==0);
  assert(r.action==SHIZUKA_RELAY_COMMITTED && r.removed==1 && get32(rsp)==sizeof cal_kept);
  assert(!memcmp(calbuf,cal_kept,sizeof cal_kept) && !b.calibration.pending);
  assert(b.transaction.phase==before.transaction.phase);
  assert(b.transaction.profile_confirmed==before.transaction.profile_confirmed);
  assert(shizuka_graph_bridge_active_delta(&b,&e)==delta);
 }
}
int main(void){
 struct shizuka_graph_bridge_result r;
 /* Ordinary DZIF GSL ports are below 0x4001. Only a positive original GPR
  * registration bound to this graph permits their use as an owner source. */
 for(unsigned gate=0;gate<4;gate++){
  init();
  if(gate==0)e.scope.flags=7;
  if(gate==1)e.scope.source_port=0;
  if(gate==2)e.scope.source_port=UINT32_MAX;
  if(gate==3)e.scope.source_port=e.scope.client_port;
  assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0 && r.action==0);
  assert(!b.frozen_context_seq && b.transaction.phase==SHIZUKA_RELAY_IDLE && get32(rsp+16)==sizeof stock);
 }
 init();assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0);
 assert(r.action==SHIZUKA_RELAY_ADVERTISE);e.scope.flags=7;put32(rsp+16,sizeof stock);
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x247d0,&r)==1 && r.action==SHIZUKA_RELAY_ABORT);
 assert(b.transaction.phase==SHIZUKA_RELAY_FAILED && get32(rsp+16)==sizeof stock);
 init();grow();
 assert(b.transaction.scope.source_port==0x2012 && b.transaction.scope.flags==15);
 assert(!shizuka_graph_bridge_open_ack(&b,&e,SHIZUKA_GRAPH_OPEN_OPCODE,e.scope.source_port+1,0));
 assert(b.transaction.phase==SHIZUKA_RELAY_FILLED);
 assert(shizuka_graph_bridge_open_ack(&b,&e,SHIZUKA_GRAPH_OPEN_OPCODE,e.scope.source_port,0));
 assert(b.open_accepted && !shizuka_graph_bridge_active_delta(&b,&e));
 assert(!shizuka_graph_bridge_confirm_profile(&b,&e,1,0x07001082,4096,44100,0));
 assert(!shizuka_graph_bridge_active_delta(&b,&e));
 assert(shizuka_graph_bridge_confirm_profile(&b,&e,1,0x07001082,4096,44100,1));
 assert(shizuka_graph_bridge_active_delta(&b,&e)==150000);
 struct shizuka_graph_bridge before=b;
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph+8,0x246e4,&r)==0 && r.action==0);
 assert(!memcmp(&before,&b,sizeof b));
 e.scope.flags=7;
 assert(!shizuka_graph_bridge_active_delta(&b,&e));
 assert(!shizuka_graph_bridge_confirm_profile(&b,&e,1,0x07001082,4096,44100,1));
 assert(b.transaction.phase==SHIZUKA_RELAY_FAILED && b.open_accepted);
 e.scope.flags=15; /* A later registration fact cannot revive this failed epoch. */
 assert(!shizuka_graph_bridge_active_delta(&b,&e));
 e.context.sequence=4;
 assert(!shizuka_graph_bridge_active_delta(&b,&e));
 assert(!shizuka_graph_bridge_confirm_profile(&b,&e,1,0x07001082,4096,44100,1));
 assert(b.transaction.phase==SHIZUKA_RELAY_FAILED && b.open_accepted);
 e.transport_ready=0;e.provider.enabled=0; /* Existing graph still needs cal filtering. */
 calsetup(cal,sizeof cal);
 before=b;
 assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,0,e.scope.graph+8,0x1e2d8,&r)==0);
 assert(!memcmp(&before,&b,sizeof b) && !memcmp(calbuf,cal,sizeof cal)); /* wrong gateway register */
 assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,0,e.scope.graph,0x1e2d8,&r)==0);
 assert(r.action==2 && r.removed==1 && get32(rsp)==sizeof cal_kept && !memcmp(calbuf,cal_kept,sizeof cal_kept));
 assert(b.transaction.phase==SHIZUKA_RELAY_FAILED);
 memset(cmdb,0,sizeof cmdb);put32(cmd,2);put64(cmd+4,(uintptr_t)ids);memset(rspb,0,sizeof rspb);
 assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,0,e.scope.graph,0x1e1c0,&r)==0 && !b.calibration.pending);
 calsetup(cal_empty,sizeof cal_empty);
 assert(after(SHIZUKA_GRAPH_ACDB_CAL,36,12,0,e.scope.graph,0x1e2d8,&r)==19 && get32(rsp)==0 && r.action==3);
 struct shizuka_relay_scope other=e.scope;other.epoch++;
 shizuka_graph_bridge_close(&b,&other);assert(b.frozen_context_seq);
 shizuka_graph_bridge_close(&b,&e.scope);assert(!b.frozen_context_seq && !b.open_accepted && !b.calibration.pending);
 for(unsigned i=0;i<9;i++){
  init();switch(i){case 0:e.context.sequence=0;break;case 1:e.context.sequence=3;break;case 2:e.context.valid=0;break;
   case 3:e.context.mode=8192;break;case 4:e.context.mode=12288;break;case 5:e.context.private_flags=10;break;
   case 6:e.context.sample_rate=96000;break;case 7:e.context.reserved[6]=1;break;case 8:e.transport_ready=0;break;}
  assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0 && r.action==0);
  assert(!b.frozen_context_seq && get32(rsp+16)==sizeof stock);
 }
 init();assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0);e.context.sequence=4;put32(rsp+16,sizeof stock);
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x247d0,&r)==1 && r.action==-1 && b.transaction.phase==5);
 init();assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0);put32(rsp+16,sizeof stock);
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,7,e.scope.graph,0x247d0,&r)==7 && b.transaction.phase==5);
 init();assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0);put32(rsp+16,sizeof stock);
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x247d0,&r)==0);memcpy(blob,stock,sizeof stock);
 put32(rsp+16,sizeof stock);put64(rsp+20,(uintptr_t)blob);e.spf_capacity=sizeof stock;
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x24e20,&r)==1 && !memcmp(blob,stock,sizeof stock));
 init();put64(cmd+4,(uintptr_t)((unsigned char*)ids+1));
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0x246e4,&r)==0 && !b.frozen_context_seq);
 init();assert(after(0,24,28,0,e.scope.graph,0x246e4,&r)==0 && !b.frozen_context_seq);
 assert(after(SHIZUKA_GRAPH_ACDB_DATA,24,28,0,e.scope.graph,0,&r)==0 && !b.frozen_context_seq);
 unrelated_calibration();
 puts("graph bridge: registered stock source0x2012, missing/lost registration and invalid ports, packed ABI, exact graph/epoch, context, size/fill, ACK/R3, unrelated SG/default/different-key calibration and error isolation, pending target query retention, no delta revival, cal after revocation, EMPTY19 and capacity/error ownership passed");
}
