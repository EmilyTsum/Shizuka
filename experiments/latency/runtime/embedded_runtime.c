/* SPDX-License-Identifier: GPL-2.0-only
 * Exact DZIF in-HAL runtime. No new GPR device, transport init/deinit, dynamic
 * library loading, or executable-memory allocation. Vendor adapters resolve
 * only the reviewed existing GSL GOT and loaded OSAL exports.
 */
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <asm/unistd.h>
#include "shizuka/hal_transport.h"
#include "shizuka/encoded_transform.h"
#include "shizuka/vendor_adapter.h"
#include "graph_bridge.h"
#include "shizuka/control_ack.h"
#include "shizuka/control_scope.h"
#include "shizuka/receive_trace.h"
#include "shizuka/data_receipt_trace.h"
#include "shizuka/init_profile.h"
#include "default_cal_context.h"
#include "activation_cookie.h"
#ifndef SHIZUKA_ATTACH
#define SHIZUKA_ATTACH 0
#endif
#ifndef SHIZUKA_RELAY
#define SHIZUKA_RELAY 0
#endif
#ifndef SHIZUKA_HAL_CONTEXT_RVA
#define SHIZUKA_HAL_CONTEXT_RVA 0
#endif
#ifndef SHIZUKA_DIAG_ZERO_TTP
#define SHIZUKA_DIAG_ZERO_TTP 0
#endif
#ifndef SHIZUKA_RELAY_RAW_ISLAND
#define SHIZUKA_RELAY_RAW_ISLAND 0
#endif
#if (SHIZUKA_DIAG_ZERO_TTP != 0 && SHIZUKA_DIAG_ZERO_TTP != 1) || (SHIZUKA_DIAG_ZERO_TTP && !SHIZUKA_RELAY)
#error Invalid diagnostic relay build
#endif
#if (SHIZUKA_RELAY_RAW_ISLAND != 0 && SHIZUKA_RELAY_RAW_ISLAND != 1) || (SHIZUKA_RELAY_RAW_ISLAND && !SHIZUKA_RELAY)
#error Invalid experimental RAW island build
#endif

struct gpr_packet {
    uint32_t header;
    uint8_t dst_domain,src_domain,client_data,reserved;
    uint32_t src_port,dst_port,token,opcode;
};
struct gpr_alloc {
    uint8_t src_domain; uint32_t src_port;
    uint8_t dst_domain; uint32_t dst_port;
    uint8_t client_data; uint32_t token,opcode,payload_size;
    struct gpr_packet **packet;
};
typedef uint32_t (*gpr_cb)(struct gpr_packet *,void *);
typedef uint32_t (*register_fn)(uint32_t,gpr_cb,void *);
typedef uint32_t (*send_fn)(struct gpr_packet *);
typedef int32_t (*acdb_fn)(uint32_t,const void *,uint32_t,void *,uint32_t);
typedef int32_t (*basic_fn)(struct gpr_packet **,void *);
typedef int32_t (*message_alloc_fn)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,
    uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,void *);
struct lease { uint64_t revision,expiry,hal_start; uint32_t hq_us,ll_us,hal_pid,owner_pid; };
struct callback_record { uint32_t port; gpr_cb callback; void *context; };
struct wait_record {
    struct shizuka_control_ack_expect expect;
    uint32_t tid,init_rate,init_mode,has_init,init_r3;
    uint8_t init_payload[59];
    uint32_t start_peer_sampled;
    int32_t start_peer_present;
};
struct message_record {
    uintptr_t message,packet,body,shmem;
    uint64_t address,epoch;
    uint32_t map,capacity,source,tid;
};
struct runtime_debug { uint8_t magic[8]; uint32_t bytes,sequence; uint8_t body[496]; };
_Static_assert(sizeof(struct runtime_debug)==512,"bounded debug record");
struct runtime {
    /* Root-readable, bounded diagnostic snapshot. It does not authorize a graph
     * or contribute to applied status. See runtime debug layout documentation. */
    struct runtime_debug debug;
    struct shizuka_data_receipt_trace data_receipts;
    uint32_t debug_pc_head;
    uint32_t initialized,stopping,worker_running,lease_valid,lock;
    uintptr_t base,osal_base,gpr_base;
    void *thread;
    uint8_t boot[16];
    uint32_t pid,stage;
    int32_t error;
    uint64_t start_ticks,status_generation,callbacks,acdb_calls,sends;
    uint32_t last_opcode,last_ack,last_event,last_format,init_accepted,open_ack,start_ack;
    uint32_t before_ttp,after_ttp,sample_rate,profile,source_port;
    uint32_t pending_vendor_fault,need_graph_close,transport_initialized;
    uint32_t transport_lock;
    uint64_t next_epoch;
    uint64_t start_peer_epoch;
    uint32_t start_peer_flags;
    uintptr_t hal_context;
    int32_t (*adapter_transform)(void *,uint32_t,uint64_t,uint8_t *,uint32_t,uint32_t,
        uint8_t *,uint32_t,uint32_t,struct shizuka_transport_transform_result *);
    struct wait_record waits[16];
    struct message_record messages[16];
    struct shizuka_receive_trace receive_trace;
    struct lease lease;
    register_fn register_port;
    send_fn send_packet;
    uint32_t (*free_packet)(struct gpr_packet *);
    uint32_t (*alloc_packet)(struct gpr_alloc *);
    uint32_t (*deregister_port)(uint32_t);
    int32_t (*thread_create)(void **,void *,void (*)(void *),void *);
    int32_t (*thread_join)(void *);
    uint32_t (*shmem_alloc)(void *);
    uint32_t (*shmem_free)(void *);
    struct callback_record callbacks_original[256];
    uint32_t callbacks_count;
    union { uint64_t align; uint8_t bytes[512*1024]; } transport;
    union { uint64_t align; uint8_t bytes[8192]; } vendor;
    struct shizuka_vendor_functions vendor_functions;
    struct shizuka_transport_host transport_host;
    struct shizuka_transport_status transport_status;
    uint8_t buffer_diagnostic[160];
    uint32_t buffer_diagnostic_kind;
    uint8_t last_buffer_diagnostic[64];
    struct shizuka_graph_bridge bridge;
    uint8_t graph_workspace[1024*1024];
};
_Static_assert(offsetof(struct runtime,data_receipts)==512,"diagnostic sidecar placement");
static struct runtime R;
extern __attribute__((visibility("hidden"))) void shizuka_runtime_acdb_gateway(void);
extern __attribute__((visibility("hidden"))) uintptr_t shizuka_runtime_strip_code_pointer(uintptr_t);
uint32_t shizuka_runtime_callback(struct gpr_packet *,void *);
void shizuka_runtime_worker(void *);

static void copy_bytes(void *to,const void *from,size_t n) {
    uint8_t *a=to;const uint8_t *b=from;for(size_t i=0;i<n;++i)a[i]=b[i];
}
static void zero_bytes(void *to,size_t n) {uint8_t *a=to;for(size_t i=0;i<n;++i)a[i]=0;}
static int equal_bytes(const void *a,const void *b,size_t n) {
    const uint8_t *x=a,*y=b;for(size_t i=0;i<n;++i)if(x[i]!=y[i])return 0;return 1;
}
static uint32_t get32(const void *p) {const uint8_t *b=p;return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static uint64_t get64(const void *p) {const uint8_t *b=p;return get32(b)|(uint64_t)get32(b+4)<<32;}
static void put32(void *p,uint32_t v) {uint8_t *b=p;for(unsigned i=0;i<4;++i)b[i]=(uint8_t)(v>>(8*i));}
static void put64(void *p,uint64_t v) {uint8_t *b=p;put32(b,(uint32_t)v);put32(b+4,(uint32_t)(v>>32));}
static uint32_t crc32(const uint8_t *p,size_t n) {
    uint32_t v=UINT32_MAX;for(size_t i=0;i<n;++i){v^=p[i];for(unsigned j=0;j<8;++j)v=(v>>1)^(0xedb88320u&(0u-(v&1)));}return ~v;
}
static long call6(long nr,long a,long b,long c,long d,long e,long f) {
    register long x8 __asm__("x8")=nr;
    register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ __volatile__("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return x0;
}
static uint64_t now_ns(void) {
    int64_t t[2]={0,0};if(call6(__NR_clock_gettime,1,(long)t,0,0,0,0))return 0;
    return (uint64_t)t[0]*1000000000u+(uint64_t)t[1];
}
static void sleep_ms(unsigned ms) {
    int64_t t[2]={ms/1000,(int64_t)(ms%1000)*1000000};call6(__NR_nanosleep,(long)t,0,0,0,0,0);
}
static void state_lock(void) {while(__atomic_exchange_n(&R.lock,1,__ATOMIC_ACQUIRE)) {}}
static void state_unlock(void) {__atomic_store_n(&R.lock,0,__ATOMIC_RELEASE);}
static void record_error(int32_t error) {
    if(error){int32_t expected=0;__atomic_compare_exchange_n(&R.error,&expected,error,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
}
/* One logical transport executor. Control calls set state; only worker.step
 * allocates/sends/transforms. Never hold state_lock while acquiring this lock.
 * Receive remains a nonblocking SPSC producer and never takes either lock. */
static void transport_lock(void) {
    while(__atomic_exchange_n(&R.transport_lock,1,__ATOMIC_ACQUIRE))
        call6(__NR_futex,(long)&R.transport_lock,128,1,0,0,0);
}
static void transport_unlock(void) {
    __atomic_store_n(&R.transport_lock,0,__ATOMIC_RELEASE);
    call6(__NR_futex,(long)&R.transport_lock,129,1,0,0,0);
}
static int read_file(const char *path,void *buffer,size_t capacity,int private_provider) {
    long fd=call6(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0,0,0);
    if(fd<0)return -1;
    struct stat s;int ok=call6(__NR_fstat,fd,(long)&s,0,0,0,0)==0&&S_ISREG(s.st_mode);
    if(private_provider)ok=ok&&s.st_uid==0&&(s.st_mode&07777)==0640&&s.st_size==(off_t)capacity;
    long bytes=ok?call6(__NR_read,fd,(long)buffer,(long)capacity,0,0,0):-1;
    call6(__NR_close,fd,0,0,0,0,0);return (int)bytes;
}
static uint64_t own_start(void) {
    char b[1024];int n=read_file("/proc/self/stat",b,sizeof(b)-1,0);if(n<1)return 0;b[n]=0;
    int at=n-1;while(at>=0&&b[at]!=')')--at;if(at<0)return 0;
    ++at;unsigned field=3;while(at<n){while(at<n&&b[at]==' ')++at;int end=at;while(end<n&&b[end]!=' ')++end;
        if(field==22){uint64_t value=0;for(int i=at;i<end;++i){if(b[i]<'0'||b[i]>'9')return 0;value=value*10+(unsigned)(b[i]-'0');}return value;}
        ++field;at=end;}
    return 0;
}
static int boot_id(uint8_t out[16]) {
    char b[40];int n=read_file("/proc/sys/kernel/random/boot_id",b,sizeof(b),0);if(n<36)return 0;
    unsigned k=0;uint32_t value=0;for(int i=0;i<36;++i){if(b[i]=='-')continue;unsigned c=(unsigned char)b[i];
        if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else return 0;
        value=(value<<4)|c;if((++k&1)==0)out[k/2-1]=(uint8_t)value;}
    return k==32;
}
static int read_lease(struct lease *out) {
    uint8_t b[128];if(read_file("/data/vendor/audio/shizuka/provider.bin",b,sizeof(b),1)!=(int)sizeof(b))return 0;
    if(!equal_bytes(b,"SHZLAT1\0",8)||get32(b+8)!=1||get32(b+12)!=128||
       !equal_bytes(b+16,R.boot,16)||!equal_bytes(b+32,shizuka_activation_cookie,16)||
       get32(b+72)!=R.pid||get64(b+80)!=R.start_ticks||get32(b+124)!=crc32(b,124))return 0;
    for(unsigned i=96;i<124;++i)if(b[i])return 0;
    uint64_t now=now_ns(),expiry=get64(b+56);
    if(!now||expiry<=now||expiry-now>2000000000u||get32(b+64)>150000||get32(b+68)>100000||!get64(b+48))return 0;
    *out=(struct lease){get64(b+48),expiry,get64(b+80),get32(b+64),get32(b+68),get32(b+72),get32(b+76)};
    return 1;
}
static int hex_word(const char **p,const char *end,uintptr_t *out) {
    uintptr_t value=0;unsigned digits=0;
    while(*p<end){unsigned c=(unsigned char)**p;
        if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else break;
        if(++digits>16||value>(UINTPTR_MAX-c)/16)return 0;
        value=value*16+c;++*p;
    }*out=value;return digits!=0;
}
static int has_suffix(const char *p,size_t n,const char *suffix,size_t sn) {
    return n>=sn&&equal_bytes(p+n-sn,suffix,sn);
}
static void maps_line(const char *line,size_t n,uintptr_t *base,int *mapped) {
    const char *p=line,*end=line+n;uintptr_t lo,hi,offset;
    if(!hex_word(&p,end,&lo)||p==end||*p++!='-'||!hex_word(&p,end,&hi)
        ||p==end||*p++!=' '||end-p<5)return;
    int writable=p[0]=='r'&&p[1]=='w';p+=4;if(p==end||*p++!=' ')return;
    if(!hex_word(&p,end,&offset))return;
    static const char name[]="/libsehbluetooth_audio_session_aidl.so";
    if(!offset&&has_suffix(line,n,name,sizeof(name)-1)) {
        if(*base&&*base!=lo){*mapped=-1;return;}*base=lo;
    }
    if(*base&&writable&&lo<=*base+SHIZUKA_HAL_CONTEXT_RVA
        &&hi>=*base+SHIZUKA_HAL_CONTEXT_RVA+sizeof(S25LatencyContext)&&*mapped>=0)*mapped=1;
}
static uintptr_t find_hal_context(void) {
    if(!SHIZUKA_HAL_CONTEXT_RVA)return 0;
    long fd=call6(__NR_openat,AT_FDCWD,(long)"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)return 0;
    char buffer[4096],line[1024];size_t used=0;uintptr_t base=0;int mapped=0,overflow=0;
    for(unsigned round=0;round<512;++round){
        long n=call6(__NR_read,fd,(long)buffer,sizeof(buffer),0,0,0);if(n<=0)break;
        for(long i=0;i<n;++i){char c=buffer[i];
            if(c=='\n'){if(!overflow)maps_line(line,used,&base,&mapped);used=0;overflow=0;}
            else if(used<sizeof(line))line[used++]=c;else overflow=1;
        }
    }
    call6(__NR_close,fd,0,0,0,0,0);
    return mapped==1?base+SHIZUKA_HAL_CONTEXT_RVA:0;
}
static int context_snapshot(S25LatencyContext *out) {
    zero_bytes(out,sizeof(*out));if(!SHIZUKA_RELAY)return 0;
    if(!R.hal_context)R.hal_context=find_hal_context();if(!R.hal_context)return 0;
    const uint32_t *p=(const uint32_t *)R.hal_context;
    for(unsigned attempt=0;attempt<3;++attempt){
        uint32_t seq=__atomic_load_n(p+3,__ATOMIC_ACQUIRE);if(!seq||(seq&1))continue;
        for(unsigned i=0;i<16;++i)((uint32_t *)out)[i]=__atomic_load_n(p+i,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(seq==__atomic_load_n(p+3,__ATOMIC_ACQUIRE)&&out->sequence==seq)return 1;
    }zero_bytes(out,sizeof(*out));return 0;
}
/* Called under transport_lock, which also owns bridge state. */
static int source_registered(uintptr_t graph,uint32_t source) {
    for(unsigned i=0;i<256;++i){const struct callback_record *c=&R.callbacks_original[i];
        if(c->port==source&&c->callback==(gpr_cb)(R.base+0x38a98)&&(uintptr_t)c->context==graph)
            return graph&&get32((const uint8_t *)graph+0xa4)==source&&get32((const uint8_t *)graph+0xa8)==2;
    }return 0;
}
static void graph_env(struct shizuka_graph_bridge_env *e,uintptr_t graph,uint32_t fresh) {
    zero_bytes(e,sizeof(*e));e->library_base=R.base;context_snapshot(&e->context);
    struct shizuka_transport_status s;zero_bytes(&s,sizeof(s));s.abi=SHIZUKA_TRANSPORT_ABI;s.bytes=sizeof(s);
    if(R.transport_initialized&&!shizuka_transport_get_status(R.transport.bytes,&s))
        e->transport_ready=s.registration_ready&&!s.error&&
            (s.phase==SHIZUKA_TRANSPORT_READY||s.phase==SHIZUKA_TRANSPORT_CLOSED||
             (R.bridge.frozen_context_seq&&(s.phase==SHIZUKA_TRANSPORT_CONFIGURING||
              s.phase==SHIZUKA_TRANSPORT_CONFIGURED||s.phase==SHIZUKA_TRANSPORT_ACTIVE||
              s.phase==SHIZUKA_TRANSPORT_PAUSING||s.phase==SHIZUKA_TRANSPORT_PAUSED)));
    state_lock();
    e->provider.enabled=R.lease_valid;e->provider.revision=R.lease.revision;
    e->provider.hq_extra_ttp_us=R.lease.hq_us;
    state_unlock();
    if(R.bridge.frozen_context_seq&&graph==R.bridge.transaction.scope.graph) {
        e->scope=R.bridge.transaction.scope;
        if(!source_registered(graph,e->scope.source_port))e->scope.flags&=~SHIZUKA_GSL_SOURCE_REGISTERED;
        e->scope.thread=(uintptr_t)call6(__NR_gettid,0,0,0,0,0,0);return;
    }
    if(!fresh||!graph)return;
    e->scope.graph=graph;e->scope.thread=(uintptr_t)call6(__NR_gettid,0,0,0,0,0,0);
    e->scope.epoch=R.next_epoch+1;e->scope.client_port=SHIZUKA_TRANSPORT_CLIENT_PORT;
    e->scope.source_port=get32((const uint8_t *)graph+0xa4);
    if(get32((const uint8_t *)graph+0xa8)!=2)return;
    e->scope.flags=SHIZUKA_GSL_EXACT_LIBRARY;
    if(source_registered(graph,e->scope.source_port))e->scope.flags|=SHIZUKA_GSL_SOURCE_REGISTERED;
    uint8_t *sg=((void *(*)(uint32_t))(R.base+0x38880))(SHIZUKA_RELAY_SG);
    if(sg&&get32(sg+0x18)==SHIZUKA_RELAY_SG){
        e->scope.open_refs=get32(sg+0x1c);e->scope.start_refs=get32(sg+0x20);e->scope.stop_refs=get32(sg+0x24);
        if(e->scope.open_refs==1&&!e->scope.start_refs&&!e->scope.stop_refs)
            e->scope.flags|=SHIZUKA_GSL_NAMESPACE_OWNED;
    }
}
static uint32_t debug_begin(void) {
    uint32_t sequence=__atomic_load_n(&R.debug.sequence,__ATOMIC_RELAXED);
    sequence=(sequence&~1u)+1;if(!sequence)sequence=1;
    __atomic_exchange_n(&R.debug.sequence,sequence,__ATOMIC_ACQ_REL);
    copy_bytes(&R.debug,"SHZDBG1\0",8);put32((uint8_t *)&R.debug+8,sizeof(R.debug));put32((uint8_t *)&R.debug+16,1);
    return sequence;
}
static void debug_end(uint32_t sequence) {
    __atomic_store_n(&R.debug.sequence,sequence==UINT32_MAX?2:sequence+1,__ATOMIC_RELEASE);
}
static void debug_pc(uint32_t pc) {
    uint32_t seq=debug_begin();put32((uint8_t *)&R.debug+328+(R.debug_pc_head++%16)*4,pc);debug_end(seq);
}
static void debug_init(const struct wait_record *w,const struct shizuka_graph_bridge_env *env,int32_t rc) {
    uint32_t seq=debug_begin();copy_bytes((uint8_t *)&R.debug+392,w->init_payload,59);
    *((uint8_t *)&R.debug+451)=(w->has_init==1?1:0)|(w->init_r3?2:0)|(!rc?4:0)
        |(w->init_rate==2?8:0)|(w->init_mode==(0x1000u|((uint32_t)w->init_payload[48]<<8)|w->init_payload[49])?16:0)
        |(env->context.mode==4096?32:0)|(env->context.sample_rate==44100?64:0);
    put32((uint8_t *)&R.debug+452,(uint32_t)rc);debug_end(seq);
}
static void debug_scope(const struct shizuka_graph_bridge_env *env,uint32_t pc,
    const void *command,uint32_t size,uintptr_t graph,int action) {
    uint32_t seq=debug_begin();put32((uint8_t *)&R.debug+20,(uint32_t)action);put64((uint8_t *)&R.debug+24,now_ns());
    put64((uint8_t *)&R.debug+32,pc);put64((uint8_t *)&R.debug+40,graph);put64((uint8_t *)&R.debug+48,R.hal_context);
    put32((uint8_t *)&R.debug+56,env->scope.source_port);put32((uint8_t *)&R.debug+60,graph?get32((uint8_t *)graph+0xa8):0);
    put32((uint8_t *)&R.debug+64,env->scope.flags);put32((uint8_t *)&R.debug+68,env->scope.open_refs);
    put32((uint8_t *)&R.debug+72,env->scope.start_refs);put32((uint8_t *)&R.debug+76,env->scope.stop_refs);
    put32((uint8_t *)&R.debug+80,R.bridge.transaction.phase);put32((uint8_t *)&R.debug+84,env->transport_ready);
    put32((uint8_t *)&R.debug+88,env->provider.enabled);put32((uint8_t *)&R.debug+92,env->context.magic==S25_LATENCY_CONTEXT_MAGIC);
    copy_bytes((uint8_t *)&R.debug+96,&env->context,sizeof(env->context));
    zero_bytes((uint8_t *)&R.debug+160,168);
    if(command&&(size==24||size==36)){
        const uint8_t *p=command;uint32_t ids=get32(p),keys=size==24?get32(p+12):env->num_gkv;
        put32((uint8_t *)&R.debug+160,ids);put32((uint8_t *)&R.debug+164,keys);
        const void *sg=(const void *)(uintptr_t)get64(p+4),*kv=size==24?(const void *)(uintptr_t)get64(p+16):env->current_gkv;
        if(ids&&ids<=128&&sg&&!((uintptr_t)sg&3))copy_bytes((uint8_t *)&R.debug+168,sg,(ids<8?ids:8)*4);
        if(keys&&keys<=64&&kv&&!((uintptr_t)kv&3))copy_bytes((uint8_t *)&R.debug+200,kv,(keys<16?keys:16)*8);
    }
    debug_end(seq);
}
static void debug_startup(const struct shizuka_transport_status *s) {
    uint32_t seq=debug_begin();uint8_t *p=(uint8_t *)&R.debug+392;
    zero_bytes(p,64);copy_bytes(p,"BST1",4);put32(p+4,s->startup_flags);
    put32(p+8,s->rd_frame_bytes);put32(p+12,s->initial_peer_acks);
    put32(p+16,s->last_rd_state);put32(p+20,s->rd_state_events);
    put32(p+24,s->pending_control);put32(p+28,s->media_format_bytes);
    put32(p+32,(uint32_t)s->error);put32(p+36,s->phase);
    put32(p+40,s->unresolved_buffers);put32(p+44,s->unresolved_mappings);
    put32(p+48,R.bridge.frozen_context_seq);put64(p+56,now_ns());
    if(s->graph_epoch&&get64(R.buffer_diagnostic+8)==s->graph_epoch){
        zero_bytes((uint8_t *)&R.debug+160,8);
        copy_bytes((uint8_t *)&R.debug+168,R.buffer_diagnostic,sizeof(R.buffer_diagnostic));
    }
    if(s->graph_epoch&&get64(R.last_buffer_diagnostic+8)==s->graph_epoch){
        copy_bytes((uint8_t *)&R.debug+96,R.last_buffer_diagnostic,sizeof(R.last_buffer_diagnostic));
        put32((uint8_t *)&R.debug+92,2);
    }
    if(s->graph_epoch){
        uint8_t *peer=(uint8_t *)&R.debug+328;
        zero_bytes(peer,64);copy_bytes(peer,"RTP1",4);put32(peer+4,64);put64(peer+8,s->graph_epoch);
        put32(peer+16,s->wr_peer_bytes);put32(peer+20,s->wr_peer_count);
        put32(peer+24,s->wr_peer_type);put32(peer+28,s->wr_peer_value);put64(peer+32,now_ns());
        put32(peer+40,SHIZUKA_RELAY_RAW_ISLAND?0xe00000e5u:0xe00000e6u);put32(peer+44,0xe00000e6u);
        put32(peer+48,s->wr_state_bytes);put32(peer+52,s->last_wr_state);put32(peer+56,s->wr_state_events);
        put32(peer+60,R.start_peer_epoch==s->graph_epoch?R.start_peer_flags:0);
    }
    debug_end(seq);
}
static void debug_buffer(uint32_t profile,uint64_t delta,const uint8_t *data,uint32_t bytes,uint32_t capacity,
    const uint8_t *metadata,uint32_t mdbytes,uint32_t mdcapacity,int32_t rc,const struct shz_encoded_result *proof) {
    uint64_t epoch=R.bridge.transaction.scope.epoch;uint8_t *p=R.buffer_diagnostic;
    if(!epoch||!data||!metadata||bytes>capacity||mdbytes>mdcapacity
        ||capacity>SHIZUKA_TRANSPORT_DATA_CAPACITY||mdcapacity>SHIZUKA_TRANSPORT_METADATA_CAPACITY)return;
    uint8_t *last=R.last_buffer_diagnostic;zero_bytes(last,sizeof(R.last_buffer_diagnostic));
    copy_bytes(last,"SHZLST1\0",8);put64(last+8,epoch);put32(last+16,bytes);put32(last+20,mdbytes);
    put32(last+24,(uint32_t)rc);
    put32(last+28,proof?shizuka_transport_wr_wire_flags(proof->wr_flags):0);put64(last+32,delta);
    copy_bytes(last+40,data,bytes<8?bytes:8);put64(last+48,proof?proof->timestamp_bits:0);
    put32(last+56,proof?proof->wrapper_count:0);put32(last+60,proof?proof->changed_wrapper_count:0);
    /* Prefer the first supplemental packet over the first timing packet, then
     * the first parser failure. Preserve their nonaudio metadata for diagnosis.
     * No encoded audio body, pointer or packet is retained. */
    uint32_t kind=rc?3u:(proof&&!proof->timing_wrapper_count?2u:1u);
    if(get64(p+8)==epoch&&R.buffer_diagnostic_kind>=kind)return;
    R.buffer_diagnostic_kind=kind;
    zero_bytes(p,sizeof(R.buffer_diagnostic));copy_bytes(p,"SHZBUF1\0",8);put64(p+8,epoch);
    put32(p+16,bytes);put32(p+20,mdbytes);put32(p+24,(uint32_t)rc);put32(p+28,profile);put64(p+32,delta);
    copy_bytes(p+40,data,bytes<8?bytes:8);copy_bytes(p+48,metadata,mdbytes<112?mdbytes:112);
}
static void publish_status(void) {
    state_lock();
    uint8_t b[256];zero_bytes(b,sizeof(b));copy_bytes(b,"SHZSTAT1",8);put32(b+8,1);put32(b+12,256);
    copy_bytes(b+16,R.boot,16);copy_bytes(b+32,shizuka_activation_cookie,16);put32(b+48,R.pid);
    put32(b+52,__atomic_load_n(&R.stage,__ATOMIC_RELAXED));put32(b+56,(uint32_t)__atomic_load_n(&R.error,__ATOMIC_RELAXED));put32(b+60,R.lease_valid);
    put64(b+64,R.lease.revision);put64(b+72,R.transport_status.graph_epoch);
    put64(b+80,SHIZUKA_DIAG_ZERO_TTP?0:R.transport_status.delta_us);put64(b+88,R.transport_status.rd_frames);put64(b+96,R.transport_status.wr_frames);
    put32(b+104,R.transport_status.unresolved_mappings);put32(b+108,R.transport_status.unresolved_buffers);
    put32(b+112,R.before_ttp);put32(b+116,R.after_ttp);put32(b+120,R.sample_rate);put32(b+124,R.profile);
    put32(b+128,0xb00000ab);put32(b+132,0xe00000e6);put32(b+136,R.source_port);put32(b+140,R.init_accepted);
    put32(b+144,R.lease.owner_pid);put32(b+148,R.transport_status.registration_ready);put64(b+152,__atomic_load_n(&R.callbacks,__ATOMIC_RELAXED));
    put64(b+160,R.acdb_calls);put64(b+168,__atomic_load_n(&R.sends,__ATOMIC_RELAXED));
    put32(b+176,__atomic_load_n(&R.initialized,__ATOMIC_ACQUIRE));put32(b+180,__atomic_load_n(&R.worker_running,__ATOMIC_ACQUIRE));
    put32(b+184,__atomic_load_n(&R.last_opcode,__ATOMIC_RELAXED));put32(b+188,R.last_ack);put32(b+192,__atomic_load_n(&R.last_event,__ATOMIC_RELAXED));put32(b+196,R.last_format);
    put64(b+200,++R.status_generation);put64(b+208,now_ns());put32(b+216,R.open_ack);put32(b+220,R.start_ack);
    put64(b+224,R.transport_status.rewritten_wrappers);
    put32(b+232,SHIZUKA_DIAG_ZERO_TTP);
    put32(b+236,SHIZUKA_RELAY_RAW_ISLAND);
    put32(b+252,crc32(b,252));
    state_unlock();
    long fd=call6(__NR_openat,AT_FDCWD,(long)"/data/vendor/audio/shizuka/status.bin",O_WRONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0,0,0);
    if(fd<0)return;struct stat s;
    /* HAL-owned pre-created regular record; never create a path as HAL. */
    long uid=call6(__NR_getuid,0,0,0,0,0,0);
    if(!call6(__NR_fstat,fd,(long)&s,0,0,0,0)&&S_ISREG(s.st_mode)&&s.st_uid==(uid_t)uid&&(s.st_mode&07777)==0640)
        call6(__NR_pwrite64,fd,(long)b,sizeof(b),0,0,0);
    call6(__NR_close,fd,0,0,0,0,0);
}

static long vendor_ioctl(void *user,int32_t fd,uint32_t request,void *argument) {
    (void)user;return call6(__NR_ioctl,fd,request,(long)argument,0,0,0);
}
static uint64_t vendor_clock(void *user) {(void)user;return now_ns()/1000000u;}
static void vendor_notify(void *user,uint32_t phase,int32_t error,uint32_t close) {
    (void)user;__atomic_store_n(&R.stage,phase,__ATOMIC_RELEASE);
    if(error)record_error(error);
    if(close)__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);
}
static void vendor_fault(void *user,uint32_t reason) {
    (void)user;__atomic_store_n(&R.pending_vendor_fault,reason,__ATOMIC_RELEASE);
}
static void vendor_transformed(void *user,int32_t error,uint32_t profile,
    uint64_t delta,const struct shz_encoded_result *result) {
    (void)user;(void)delta;
    if(error||!result)return;
    state_lock();
    if(result->timing_wrapper_count){R.before_ttp=result->last_ttp_before_us;R.after_ttp=result->last_ttp_after_us;}
    R.profile=profile;state_unlock();
}
static int32_t runtime_transform(void *user,uint32_t profile,uint64_t delta,
    uint8_t *data,uint32_t bytes,uint32_t capacity,uint8_t *metadata,uint32_t mdbytes,
    uint32_t mdcapacity,struct shizuka_transport_transform_result *out) {
    struct shizuka_graph_bridge_env env;
    graph_env(&env,R.bridge.transaction.scope.graph,0);
    if(!env.provider.enabled||!R.init_accepted||profile!=1
        ||delta!=R.bridge.transaction.provider.hq_extra_ttp_us){record_error(-359);return -1;}
    /* The separately identified diagnostic image retains every owner/profile
     * gate above, but changes no packet TTP. It can never report applied delay. */
    const uint64_t effective_delta=SHIZUKA_DIAG_ZERO_TTP?0:delta;
    struct shz_encoded_result proof;
    int rc=shz_encoded_validate(data,bytes,capacity,metadata,mdbytes,mdcapacity,effective_delta,profile,&proof);
    debug_buffer(profile,effective_delta,data,bytes,capacity,metadata,mdbytes,mdcapacity,rc,rc?0:&proof);
    if(rc){record_error(-360-rc);return rc;}
    if(!shizuka_graph_bridge_confirm_profile(&R.bridge,&env,1,0x07001082,
        env.context.mode,env.context.sample_rate,1)
        ||shizuka_graph_bridge_active_delta(&R.bridge,&env)!=delta){record_error(-367);return -1;}
    return R.adapter_transform(user,profile,effective_delta,data,bytes,capacity,metadata,mdbytes,mdcapacity,out);
}
static int transport_initialize(void) {
    if(shizuka_vendor_adapter_bytes()>sizeof(R.vendor.bytes)||shizuka_transport_context_bytes()>sizeof(R.transport.bytes))return -110;
    /* Volatile field stores avoid linker-concealed absolute pointer tables. */
    volatile struct shizuka_vendor_functions *v=&R.vendor_functions;
    v->abi=SHIZUKA_VENDOR_ADAPTER_ABI;v->bytes=sizeof(*v);v->abi_profile=SHIZUKA_VENDOR_DZIF_LAYOUT_V1;v->identity_verified=1;
    v->user=0;v->gpr_register=(uint32_t (*)(uint32_t,shizuka_vendor_gpr_callback,void *))R.register_port;
    v->gpr_deregister=R.deregister_port;v->gpr_alloc=(uint32_t (*)(struct shizuka_vendor_gpr_alloc *))R.alloc_packet;
    v->gpr_send=(uint32_t (*)(struct shizuka_vendor_gpr_packet *))R.send_packet;
    v->gpr_free=(uint32_t (*)(struct shizuka_vendor_gpr_packet *))R.free_packet;
    v->shmem_alloc=(int32_t (*)(struct shizuka_vendor_shmem *))R.shmem_alloc;
    v->shmem_free=(int32_t (*)(struct shizuka_vendor_shmem *))R.shmem_free;
    v->callback_entry=(shizuka_vendor_gpr_callback)shizuka_runtime_callback;
    v->raw_ioctl=vendor_ioctl;v->clock_ms=vendor_clock;v->notify=vendor_notify;v->receive_error=vendor_fault;v->transform_result=vendor_transformed;
    int result=shizuka_vendor_adapter_init(R.vendor.bytes,sizeof(R.vendor.bytes),&R.vendor_functions);
    if(result)return result;
    result=shizuka_vendor_make_host(R.vendor.bytes,&R.transport_host);if(result)return result;
    R.adapter_transform=R.transport_host.transform;R.transport_host.transform=runtime_transform;
    struct shizuka_transport_options options;
    zero_bytes(&options,sizeof(options));options.abi=SHIZUKA_TRANSPORT_ABI;options.bytes=sizeof(options);
    options.token_seed=0x535a0000u^R.pid^(uint32_t)now_ns();if(!options.token_seed)options.token_seed=1;
    options.control_timeout_ms=2000;options.buffer_timeout_ms=3000;
    options.hal_generation=R.start_ticks;options.dsp_generation=1;
    result=shizuka_transport_init(R.transport.bytes,sizeof(R.transport.bytes),&R.transport_host,&options);
    if(result)return result;
    result=shizuka_transport_attach(R.transport.bytes);if(result)return result;
    R.transport_initialized=1;return 0;
}

/* The attach checkpoint uses only the existing HAL registry and reader.
 * Graph insertion remains disabled until the graph owner is joined. */
void shizuka_runtime_worker(void *unused) {
    (void)unused;__atomic_store_n(&R.worker_running,1,__ATOMIC_RELEASE);
    uint64_t last_status=0;uint32_t previous_lease=0;
    while(!__atomic_load_n(&R.stopping,__ATOMIC_ACQUIRE)){
        struct lease lease;uint32_t valid=(uint32_t)read_lease(&lease);
        state_lock();R.lease_valid=valid;if(valid)R.lease=lease;state_unlock();
        transport_lock();
        if(SHIZUKA_ATTACH&&valid&&!R.transport_initialized&&!__atomic_load_n(&R.error,__ATOMIC_ACQUIRE)){
            int error=transport_initialize();if(error)record_error(error);
        }
        if(R.transport_initialized){
            uint32_t fault=__atomic_exchange_n(&R.pending_vendor_fault,0,__ATOMIC_ACQ_REL);
            if(fault){record_error(-200-(int32_t)fault);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);}
            if((previous_lease&&!valid)||fault){
                struct shizuka_transport_status status;zero_bytes(&status,sizeof(status));
                status.abi=SHIZUKA_TRANSPORT_ABI;status.bytes=sizeof(status);
                if(!shizuka_transport_get_status(R.transport.bytes,&status)&&status.graph_epoch)
                    shizuka_transport_graph_stage(R.transport.bytes,status.graph_epoch,SHIZUKA_GRAPH_STOPPING,0);
            }
            shizuka_transport_step(R.transport.bytes);
            uint8_t trace[SHIZUKA_RECEIVE_TRACE_BYTES];
            if(shizuka_receive_trace_snapshot(&R.receive_trace,trace,sizeof(trace))==sizeof(trace)){
                uint32_t seq=debug_begin();copy_bytes((uint8_t *)&R.debug+456,trace,sizeof(trace));debug_end(seq);
            }
            struct shizuka_transport_status status;zero_bytes(&status,sizeof(status));
            status.abi=SHIZUKA_TRANSPORT_ABI;status.bytes=sizeof(status);
            if(!shizuka_transport_get_status(R.transport.bytes,&status)){
                debug_startup(&status);state_lock();R.transport_status=status;R.last_format=status.media_format_bytes;state_unlock();
            }
        }else __atomic_store_n(&R.stage,valid?2u:1u,__ATOMIC_RELAXED);
        transport_unlock();previous_lease=valid;
        uint64_t now=now_ns();if(now-last_status>=100000000){publish_status();last_status=now;}
        sleep_ms(SHIZUKA_ATTACH?1:20);
    }
    int detached=!R.transport_initialized;uint64_t until=now_ns()+UINT64_C(2500000000);
    while(!detached&&now_ns()<until){
        transport_lock();
        struct shizuka_transport_status status;zero_bytes(&status,sizeof(status));status.abi=SHIZUKA_TRANSPORT_ABI;status.bytes=sizeof(status);
        shizuka_transport_get_status(R.transport.bytes,&status);
        if(status.graph_epoch&&status.phase!=SHIZUKA_TRANSPORT_DRAINING&&status.phase!=SHIZUKA_TRANSPORT_CLOSED&&status.phase!=SHIZUKA_TRANSPORT_UNCERTAIN)
            shizuka_transport_graph_stage(R.transport.bytes,status.graph_epoch,SHIZUKA_GRAPH_STOPPING,0);
        shizuka_transport_step(R.transport.bytes);shizuka_transport_get_status(R.transport.bytes,&status);
        int eligible=(!status.graph_epoch||status.phase==SHIZUKA_TRANSPORT_CLOSED)&&!status.pending_control
            &&!status.unresolved_mappings&&!status.unresolved_buffers&&!status.unresolved_allocations;
        int rc=eligible?shizuka_transport_detach(R.transport.bytes):SHIZUKA_TRANSPORT_BAD_STATE;
        if(eligible&&rc){record_error(-331);until=0;}
        detached=eligible&&!rc;
        state_lock();R.transport_status=status;state_unlock();transport_unlock();
        if(!detached)sleep_ms(1);
    }
    __atomic_store_n(&R.stage,detached?SHIZUKA_TRANSPORT_DETACHED:SHIZUKA_TRANSPORT_UNCERTAIN,__ATOMIC_RELAXED);
    if(!detached)record_error(-330);
    state_lock();R.lease_valid=0;state_unlock();
    publish_status();__atomic_store_n(&R.worker_running,0,__ATOMIC_RELEASE);
}
static uintptr_t got(unsigned offset) {return *(volatile uintptr_t *)(R.base+offset);}
static void bootstrap(void) {
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&R.initialized,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    R.base=(uintptr_t)&shizuka_runtime_acdb_gateway-0x48000;
    R.pid=(uint32_t)call6(__NR_getpid,0,0,0,0,0,0);R.start_ticks=own_start();
    if(!R.start_ticks||!boot_id(R.boot)){R.error=-101;publish_status();return;}
    R.register_port=(register_fn)got(0x3c370);R.send_packet=(send_fn)got(0x3c580);
    R.free_packet=(uint32_t (*)(struct gpr_packet *))got(0x3c568);
    R.alloc_packet=(uint32_t (*)(struct gpr_alloc *))got(0x3c570);
    R.deregister_port=(uint32_t (*)(uint32_t))got(0x3c3e8);
    R.shmem_alloc=(uint32_t (*)(void *))got(0x3c738);R.shmem_free=(uint32_t (*)(void *))got(0x3c748);
    R.osal_base=(uintptr_t)R.shmem_alloc-0x10a90;R.gpr_base=(uintptr_t)R.free_packet-0x116a8;
    if((R.osal_base&4095)||(R.gpr_base&4095)){R.error=-102;return;}
    R.thread_create=(int32_t (*)(void **,void *,void (*)(void *),void *))(R.osal_base+0x10780);
    R.thread_join=(int32_t (*)(void *))(R.osal_base+0x10a20);
    struct {const char *name;uint32_t stack;int32_t priority;} attr={0,262144,0};
    int32_t result=R.thread_create(&R.thread,&attr,shizuka_runtime_worker,0);
    if(!result)__atomic_store_n(&R.initialized,2,__ATOMIC_RELEASE);
    else record_error(result);
}

uint32_t shizuka_runtime_callback(struct gpr_packet *packet,void *context) {
    __atomic_fetch_add(&R.callbacks,1,__ATOMIC_RELAXED);
    if(packet){
        shizuka_data_receipt_trace_capture(&R.data_receipts,
            __atomic_load_n(&R.next_epoch,__ATOMIC_ACQUIRE),packet,packet->header>>8,now_ns()/1000000u);
        __atomic_store_n(&R.last_event,packet->opcode,__ATOMIC_RELAXED);
        /* The existing GPR allocator/reader owns the accessible packet size;
         * this is a bounded diagnostic copy, never transport authorization. */
        /* Keep media/topology/specialized responses visible after later normal
         * successful command ACKs. A malformed or negative basic still wins. */
        if(packet->opcode!=0x02001005||packet->header!=0x2060||get32((const uint8_t *)packet+28))
            shizuka_receive_trace_capture(&R.receive_trace,packet,packet->header>>8);
    }
    shizuka_vendor_receive(context,(struct shizuka_vendor_gpr_packet *)packet);
    return 0;
}
uint32_t shizuka_runtime_register(uint32_t port,gpr_cb callback,void *context,register_fn original) {
    uint32_t result=original(port,callback,context);if(!result)bootstrap();
    if(SHIZUKA_RELAY&&!result&&context&&callback==(gpr_cb)(R.base+0x38a98)){
        transport_lock();
        if(R.bridge.frozen_context_seq&&R.bridge.transaction.scope.source_port==port){
            record_error(-343);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);
            if(R.bridge.open_accepted)shizuka_transport_graph_stage(R.transport.bytes,R.bridge.transaction.scope.epoch,SHIZUKA_GRAPH_STOPPING,0);
        }else{
            int slot=-1;for(unsigned i=0;i<256;++i){
                if(R.callbacks_original[i].port==port){slot=(int)i;break;}
                if(!R.callbacks_original[i].port&&slot<0)slot=(int)i;
            }
            if(slot>=0)R.callbacks_original[slot]=(struct callback_record){port,callback,context};
        }transport_unlock();
    }return result;
}
uint32_t shizuka_runtime_deregister(uint32_t port,uint32_t (*original)(uint32_t)) {
    if(SHIZUKA_RELAY){transport_lock();
        for(unsigned i=0;i<256;++i)if(R.callbacks_original[i].port==port)zero_bytes(&R.callbacks_original[i],sizeof(R.callbacks_original[i]));
        if(R.bridge.frozen_context_seq&&R.bridge.transaction.scope.source_port==port){
            record_error(-342);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);
            if(R.bridge.open_accepted)shizuka_transport_graph_stage(R.transport.bytes,R.bridge.transaction.scope.epoch,SHIZUKA_GRAPH_STOPPING,0);
        }transport_unlock();
    }
    return original(port);
}
int32_t shizuka_runtime_message_alloc(uint32_t opcode,uint32_t source,uint32_t destination,
    uint32_t gpr_bytes,uint32_t token,uint32_t domain,uint32_t body_bytes,uint32_t inband,
    uint32_t subsystem_mask,uint32_t flags,uint32_t platform,void *message,message_alloc_fn original) {
    int32_t rc=original(opcode,source,destination,gpr_bytes,token,domain,body_bytes,inband,subsystem_mask,flags,platform,message);
    if(!SHIZUKA_RELAY||rc||!message||((uintptr_t)message&7)||opcode!=0x01001004
        ||destination!=1||domain!=2||gpr_bytes!=16||!body_bytes||body_bytes>SHIZUKA_CONTROL_MAX_PACKET_BYTES)return rc;
    transport_lock();
    if(R.bridge.frozen_context_seq&&R.bridge.transaction.scope.source_port==source){
        const uint8_t *m=message;
        uintptr_t packet=(uintptr_t)get64(m),body=(uintptr_t)get64(m+48),shmem=(uintptr_t)get64(m+8);
        uint32_t map=get32(m+24);
        if(packet&&body&&shmem&&map&&body==(uintptr_t)get64(m+16)&&body<=UINTPTR_MAX-body_bytes){
            int slot=-1;for(unsigned i=0;i<16;++i){
                if(R.messages[i].message==(uintptr_t)message){slot=-2;break;}
                if(!R.messages[i].message&&slot<0)slot=(int)i;
            }
            if(slot>=0)R.messages[slot]=(struct message_record){.message=(uintptr_t)message,.packet=packet,.body=body,.shmem=shmem,
                .address=get64(m+32),.epoch=R.bridge.transaction.scope.epoch,.map=map,.capacity=body_bytes,.source=source,
                .tid=(uint32_t)call6(__NR_gettid,0,0,0,0,0,0)};
            else{record_error(-341);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);}
        }
    }transport_unlock();return rc;
}
int32_t shizuka_runtime_message_free(void *message,int32_t (*original)(void *)) {
    if(SHIZUKA_RELAY){transport_lock();
        for(unsigned i=0;i<16;++i)if(R.messages[i].message==(uintptr_t)message)zero_bytes(&R.messages[i],sizeof(R.messages[i]));
        transport_unlock();}
    return original(message);
}
/* GSL's allocator owns the outgoing packet. For OOB CLOSE, only a tracked
 * normal GSL allocation and its still-live CPU view may supply the body. */
static int command_target(const struct gpr_packet *packet,uint32_t tid) {
    uint32_t capacity=packet->header>>8;
    int target=shizuka_control_scope_target(packet,capacity);if(target>=0)return target;
    if(packet->opcode!=0x01001004||capacity!=40||packet->header!=0x2860)return -1;
    const uint8_t *p=(const uint8_t *)packet;
    for(unsigned i=0;i<16;++i){const struct message_record *r=&R.messages[i];
        if(r->packet!=(uintptr_t)packet||r->tid!=tid||r->source!=packet->src_port||r->epoch!=R.bridge.transaction.scope.epoch)continue;
        const uint8_t *m=(const uint8_t *)r->message;
        if(get64(m)!=(uintptr_t)packet||get64(m+8)!=r->shmem||get64(m+16)!=r->body||get64(m+48)!=r->body
            ||get32(m+24)!=r->map||get64(m+32)!=r->address||get32(m+56)!=2
            ||get32(p+24)!=(uint32_t)r->address||get32(p+28)!=(uint32_t)(r->address>>32)||get32(p+32)!=r->map)return -1;
        return shizuka_control_scope_target_owned_body(packet,capacity,(const void *)r->body,r->capacity);
    }return -1;
}
uint32_t shizuka_runtime_send(struct gpr_packet *packet,send_fn original) {
    __atomic_fetch_add(&R.sends,1,__ATOMIC_RELAXED);if(packet)__atomic_store_n(&R.last_opcode,packet->opcode,__ATOMIC_RELAXED);
    uint32_t tid=(uint32_t)call6(__NR_gettid,0,0,0,0,0,0);
    if(packet&&SHIZUKA_RELAY){state_lock();for(unsigned i=0;i<16;++i){struct wait_record *w=&R.waits[i];
        if(w->tid==tid&&packet->opcode==w->expect.opcode&&packet->src_port==w->expect.source&&packet->dst_port==w->expect.destination
            &&packet->src_domain==w->expect.source_domain&&packet->dst_domain==w->expect.destination_domain){w->expect.token=packet->token;w->expect.sent=1;break;}}
        state_unlock();}
    return original(packet);
}
int32_t shizuka_runtime_signal(void *signal,uint32_t flags,int32_t status,struct gpr_packet *packet,
    int32_t (*original)(void *,uint32_t,int32_t,struct gpr_packet *)) {
    if(SHIZUKA_RELAY){
        struct shizuka_control_ack_expect *active[16];size_t count=0;
        state_lock();
        for(unsigned i=0;i<16;++i)if(R.waits[i].tid)active[count++]=&R.waits[i].expect;
        size_t available=packet&&packet->header==0x2060?32:0;
        shizuka_control_ack_observe_many(active,count,(uintptr_t)signal,flags,status,packet,available);
        state_unlock();
    }
    return original(signal,flags,status,packet);
}
static void parse_init(struct wait_record *w,const struct gpr_packet *packet) {
    uint32_t total=packet->header>>8;
    if(packet->opcode!=0x01001006||packet->header%256!=0x60||total<40||total>16408)return;
    const uint8_t *p=(const uint8_t *)packet+24;
    uint32_t bytes=get32(p+12);
    if(get32(p)||get32(p+4)||get32(p+8)||bytes>total-40)return;
    p+=16;uint32_t at=0;
    while(at<bytes){
        if(bytes-at<16)return;
        uint32_t size=get32(p+at+8);uint64_t step=16+(((uint64_t)size+7)&~UINT64_C(7));
        if(step>bytes-at)return;
        if(get32(p+at)==0x43fb&&get32(p+at+4)==0x08001183){
            if(w->has_init||size!=59||get32(p+at+12)){w->has_init=2;return;}
            w->has_init=1;w->init_rate=get32(p+at+16);w->init_mode=get32(p+at+16+0x24);
            w->init_r3=p[at+16+48]==1&&(p[at+16+49]&0x82)==0x82;
            copy_bytes(w->init_payload,p+at+16,sizeof(w->init_payload));
        }at+=(uint32_t)step;
    }
}
static int wait_configured(uint64_t epoch) {
    uint64_t until=now_ns()+UINT64_C(2500000000);
    while(now_ns()<until){
        struct shizuka_transport_status s;zero_bytes(&s,sizeof(s));s.abi=SHIZUKA_TRANSPORT_ABI;s.bytes=sizeof(s);
        transport_lock();int rc=shizuka_transport_get_status(R.transport.bytes,&s);transport_unlock();
        if(rc||s.graph_epoch!=epoch||s.error||s.need_graph_close)return 0;
        if(s.configuration_ready)return 1;
        sleep_ms(1);
    }return 0;
}
static int wait_prepared_reads(uint64_t epoch) {
    /* The RAW/RAT output can discard startup metadata at its first tick if no
     * READ is queued. Hold the already-acknowledged PREPARE caller here, with
     * neither runtime lock held, while the existing worker queues its leases.
     * This is not a START acknowledgement or evidence of encoded output. */
    uint64_t until=now_ns()+UINT64_C(2500000000);
    while(now_ns()<until){
        if(__atomic_load_n(&R.stopping,__ATOMIC_ACQUIRE)
            ||__atomic_load_n(&R.need_graph_close,__ATOMIC_ACQUIRE)
            ||__atomic_load_n(&R.error,__ATOMIC_ACQUIRE))return 0;
        transport_lock();
        int rc=shizuka_transport_prepared_reads_ready(R.transport.bytes,epoch);
        transport_unlock();
        if(rc)return rc==1;
        sleep_ms(1);
    }return 0;
}
int32_t shizuka_runtime_basic(struct gpr_packet **packet,void *signal,basic_fn original) {
    if(!SHIZUKA_RELAY||!packet||!*packet)return original(packet,signal);
    struct wait_record cached;zero_bytes(&cached,sizeof(cached));
    cached.tid=(uint32_t)call6(__NR_gettid,0,0,0,0,0,0);
    cached.expect.signal=(uintptr_t)signal;
    cached.expect.opcode=(*packet)->opcode;cached.expect.source=(*packet)->src_port;cached.expect.destination=(*packet)->dst_port;
    cached.expect.source_domain=(*packet)->src_domain;cached.expect.destination_domain=(*packet)->dst_domain;
    parse_init(&cached,*packet);
    transport_lock();uint64_t epoch=R.bridge.transaction.scope.epoch;
    int owned=R.bridge.frozen_context_seq&&cached.expect.source==R.bridge.transaction.scope.source_port;
    if(owned&&cached.expect.opcode>=0x01001001&&cached.expect.opcode<=0x01001004){
        int target=command_target(*packet,cached.tid);
        if(target!=1){
            if(target<0){
                record_error(-340);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);
                if(R.bridge.open_accepted)shizuka_transport_graph_stage(R.transport.bytes,epoch,SHIZUKA_GRAPH_STOPPING,0);
            }owned=0;
        }else if(cached.expect.opcode==0x01001002){
            cached.start_peer_sampled=1;
            cached.start_peer_present=shizuka_control_scope_start_peer_present(*packet,(*packet)->header>>8);
        }
    }
    if(owned&&R.bridge.open_accepted&&(cached.expect.opcode==0x01001003||cached.expect.opcode==0x01001004))
        shizuka_transport_graph_stage(R.transport.bytes,epoch,
            cached.expect.opcode==0x01001003?SHIZUKA_GRAPH_PAUSING:SHIZUKA_GRAPH_STOPPING,0);
    transport_unlock();
    if(!owned)return original(packet,signal);
    if((cached.has_init||cached.expect.opcode==0x01001001||cached.expect.opcode==0x01001002)&&!wait_configured(epoch)){
        record_error(-344);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);return 1;
    }
    int slot=-1;state_lock();for(unsigned i=0;i<16;++i)if(!R.waits[i].tid){R.waits[i]=cached;slot=(int)i;break;}state_unlock();
    if(slot<0){record_error(-345);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);return 1;}
    int32_t rc=original(packet,signal);
    state_lock();cached=R.waits[slot];zero_bytes(&R.waits[slot],sizeof(R.waits[slot]));state_unlock();
    uint32_t observed_start_ack=!rc&&cached.expect.ack==1&&!cached.expect.ack_status;
    if(!rc&&(cached.expect.ack!=1||cached.expect.ack_status)){record_error(-346);rc=1;}
    int await_prepared_reads=0;
    transport_lock();
    if(R.bridge.transaction.scope.epoch==epoch&&R.bridge.transaction.scope.source_port==cached.expect.source){
        struct shizuka_graph_bridge_env env;graph_env(&env,R.bridge.transaction.scope.graph,0);
        state_lock();R.last_ack=cached.expect.opcode;state_unlock();
        if(cached.expect.opcode==0x01001000){
            int accepted=shizuka_graph_bridge_open_ack(&R.bridge,&env,cached.expect.opcode,cached.expect.source,rc);
            if(accepted){
                struct shizuka_transport_graph g;zero_bytes(&g,sizeof(g));
                g.abi=SHIZUKA_TRANSPORT_ABI;g.bytes=sizeof(g);g.graph_epoch=epoch;g.hal_generation=R.start_ticks;g.dsp_generation=1;
                g.delta_us=R.bridge.transaction.provider.hq_extra_ttp_us;g.profile=1;
                g.subgraph_id=SHIZUKA_RELAY_SG;g.container_id=SHIZUKA_RELAY_CONTAINER;
                g.parent_client_id=SHIZUKA_TRANSPORT_CLIENT_PORT;g.rd_miid=SHIZUKA_RELAY_RD;g.wr_miid=SHIZUKA_RELAY_WR;
                g.fresh_owned_graph=g.endpoint_namespace_owned=g.original_imcl_preserved=g.olc_parent_protocol_reviewed=g.open_acknowledged=1;
                rc=shizuka_transport_bind_graph(R.transport.bytes,&g);
                R.start_peer_epoch=epoch;R.start_peer_flags=0;
                state_lock();R.open_ack=!rc;R.init_accepted=R.start_ack=R.sample_rate=R.profile=0;
                R.before_ttp=R.after_ttp=0;state_unlock();
            }else if(!rc)rc=1;
        }else if(cached.has_init){
            debug_init(&cached,&env,rc);
            if(rc){record_error(-351);}
            else if(cached.has_init!=1){record_error(-347);rc=1;}
            else if(!cached.init_r3){record_error(-348);rc=1;}
            else if(cached.init_rate!=2){record_error(-349);rc=1;}
            else if(!shizuka_init_profile_hq_r3(cached.init_payload,sizeof(cached.init_payload),sizeof(cached.init_payload),&env.context,0)){
                record_error(-350);rc=1;
            }else if(env.context.sequence!=R.bridge.frozen_context_seq||env.context.config_fingerprint!=R.bridge.frozen_context.config_fingerprint){
                record_error(-352);rc=1;
            }
            else rc=shizuka_transport_attest_profile(R.transport.bytes,epoch,1,1);
            state_lock();R.init_accepted=!rc;if(!rc)R.sample_rate=44100;state_unlock();
            shizuka_graph_bridge_confirm_profile(&R.bridge,&env,!rc,0x07001082,env.context.mode,env.context.sample_rate,0);
        }else if(!rc&&cached.expect.opcode>=0x01001001&&cached.expect.opcode<=0x01001004){
            uint32_t stage=0;
            if(cached.expect.opcode==0x01001001)stage=SHIZUKA_GRAPH_PREPARED;
            if(cached.expect.opcode==0x01001002)stage=SHIZUKA_GRAPH_STARTED;
            if(cached.expect.opcode==0x01001003)stage=SHIZUKA_GRAPH_STOPPED;
            if(cached.expect.opcode==0x01001004)stage=SHIZUKA_GRAPH_CLOSED;
            if((cached.expect.opcode==0x01001001||cached.expect.opcode==0x01001002)&&!R.init_accepted)rc=1;
            if(!rc)rc=shizuka_transport_graph_stage(R.transport.bytes,epoch,stage,0);
            /* Only the first fresh PREPARE needs this barrier. start_ack stays
             * set across ordinary STOP/resume and resets on the next OPEN. */
            if(SHIZUKA_RELAY_RAW_ISLAND&&!rc&&!R.start_ack&&cached.expect.opcode==0x01001001){
                rc=shizuka_transport_prepare_reads(R.transport.bytes,epoch);
                await_prepared_reads=!rc;
            }
            if(cached.expect.opcode==0x01001002){state_lock();R.start_ack=!rc;state_unlock();}
            if(cached.expect.opcode==0x01001004&&!rc){shizuka_graph_bridge_close(&R.bridge,&env.scope);state_lock();R.open_ack=R.init_accepted=R.start_ack=0;state_unlock();}
        }
        if(cached.start_peer_sampled){
            /* Observation of this exact owned START only. An absent SG6c does
             * not say whether a separate stock command started that peer. */
            R.start_peer_epoch=epoch;
            R.start_peer_flags=1u|(cached.start_peer_present>=0?2u:0u)
                |(cached.start_peer_present==1?4u:0u)
                |(observed_start_ack?8u:0u);
        }
        if(rc){record_error(rc);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);}
    }
    transport_unlock();
    if(await_prepared_reads&&!wait_prepared_reads(epoch)){
        record_error(-368);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);return 1;
    }
    return rc;
}
int32_t shizuka_runtime_acdb(uint32_t opcode,const void *command,uint32_t command_size,
    void *response,uint32_t response_size,void *graph,uintptr_t caller,acdb_fn original,
    const void *cal_node,uintptr_t cal_frame) {
    size_t spf=0,cal=0;
    if(response&&response_size==28&&opcode==SHIZUKA_GRAPH_ACDB_DATA)spf=get32((const uint8_t *)response+16);
    if(response&&response_size==12&&opcode==SHIZUKA_GRAPH_ACDB_CAL)cal=get32(response);
    int32_t rc=original(opcode,command,command_size,response,response_size);
    __atomic_fetch_add(&R.acdb_calls,1,__ATOMIC_RELAXED);
    if(!SHIZUKA_RELAY||!R.base||caller<R.base||caller-R.base>UINT32_MAX)return rc;
    uint32_t pc=(uint32_t)(caller-R.base);
    transport_lock();debug_pc(pc);transport_unlock();
    if(!((opcode==SHIZUKA_GRAPH_ACDB_DATA&&(pc==0x246e4||pc==0x247d0||pc==0x24e20))||
        (opcode==SHIZUKA_GRAPH_ACDB_CAL&&(pc==0x1e1c0||pc==0x1e2d8))))return rc;
    transport_lock();struct shizuka_graph_bridge_env env;
    graph_env(&env,(uintptr_t)graph,opcode==SHIZUKA_GRAPH_ACDB_DATA);
    env.spf_capacity=spf;env.cal_capacity=cal;
    if(opcode==SHIZUKA_GRAPH_ACDB_CAL&&cal_node&&R.bridge.frozen_context_seq
        &&(uintptr_t)graph==R.bridge.transaction.scope.graph&&!rc){
        env.num_gkv=get32((const uint8_t *)cal_node+0x18);
        env.current_gkv=(const struct shizuka_gsl_key *)(uintptr_t)get64((const uint8_t *)cal_node+0x20);
        if(!env.num_gkv){
            struct shizuka_default_cal_view view;
            if(shizuka_default_cal_context_view(&R.bridge,&env,pc,command,command_size,response,response_size,
                (uintptr_t)cal_node,cal_frame,shizuka_runtime_strip_code_pointer,&view)){
                env.current_gkv=view.keys;env.num_gkv=view.num_keys;
            }
        }
    }
    struct shizuka_graph_bridge_result result;
    rc=shizuka_graph_bridge_after_acdb(&R.bridge,&env,opcode,command,command_size,response,response_size,
        rc,(uintptr_t)graph,pc,R.graph_workspace,sizeof(R.graph_workspace),&result);
    debug_scope(&env,pc,command,command_size,(uintptr_t)graph,result.action);
    if(R.bridge.frozen_context_seq){__atomic_store_n(&R.next_epoch,R.bridge.transaction.scope.epoch,__ATOMIC_RELEASE);
        state_lock();R.source_port=R.bridge.transaction.scope.source_port;state_unlock();}
    if(result.action==SHIZUKA_RELAY_ABORT){record_error(rc);__atomic_store_n(&R.need_graph_close,1,__ATOMIC_RELEASE);}
    transport_unlock();return rc;
}
uint32_t shizuka_runtime_deinit(uint32_t (*original)(void)) {
    __atomic_store_n(&R.stopping,1,__ATOMIC_RELEASE);if(R.thread)R.thread_join(R.thread);
    R.thread=0;return original();
}
