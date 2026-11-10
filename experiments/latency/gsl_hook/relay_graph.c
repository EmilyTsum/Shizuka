/* SPDX-License-Identifier: GPL-2.0-only
 * Bounded freestanding ACDB/SPF transformer. No imports, heap, global state,
 * GPR operation, provider read, graph command, or endpoint IO lives here.
 */
#include "relay_graph.h"

#define SG_CFG UINT32_C(0x08001001)
#define CONTAINER_CFG UINT32_C(0x08001000)
#define MODULES UINT32_C(0x08001002)
#define PROPS UINT32_C(0x08001003)
#define CONNS UINT32_C(0x08001004)
#define CTRL UINT32_C(0x08001061)
#define PARENT UINT32_C(0x080010cb)
#define PORTS UINT32_C(0x08001015)
#define ENCODER UINT32_C(0x43fb)
#define ABSENT ((size_t)-1)

struct parameter { size_t start,data,size,total; uint32_t miid,pid; };
struct plan {
    size_t end,parent,encoder_parent,modules,rd_mid,wr_mid;
    size_t properties,connections,rd_ports,wr_ports;
};

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void copy(uint8_t *to,const uint8_t *from,size_t n)
{ for(size_t i=0;i<n;i++)to[i]=from[i]; }
static int span(size_t pos,size_t n,size_t end) { return pos<=end && n<=end-pos; }
static int client_valid(uint32_t client)
{ return client>=0x4001 && client!=UINT32_MAX && client!=SHIZUKA_RELAY_RD && client!=SHIZUKA_RELAY_WR; }

static int next(const uint8_t *in,size_t size,size_t *position,struct parameter *p)
{
    size_t pos=*position;
    if(!span(pos,16,size))return 0;
    uint32_t n=u32(in+pos+8);
    if(n>SHIZUKA_RELAY_MAX_SPF || u32(in+pos+12))return 0;
    size_t padded=((size_t)n+7)&~(size_t)7;
    if(!span(pos+16,padded,size))return 0;
    p->start=pos;p->data=pos+16;p->size=n;p->total=16+padded;
    p->miid=u32(in+pos);p->pid=u32(in+pos+4);
    *position=pos+p->total;
    return 1;
}

static int sg_properties(const uint8_t *d,size_t n)
{
    if(n<12 || u32(d)!=1 || u32(d+4)!=SHIZUKA_RELAY_SG)return 0;
    uint32_t count=u32(d+8),direction=0,scenario=0;size_t pos=12;
    if(count>64)return 0;
    for(uint32_t i=0;i<count;i++) {
        if(!span(pos,8,n))return 0;
        uint32_t pid=u32(d+pos),length=u32(d+pos+4);pos+=8;
        if(!span(pos,length,n))return 0;
        if(pid==0x0800100f) { if(length!=4 || direction || u32(d+pos)!=2)return 0;direction=1; }
        if(pid==0x08001010) { if(length!=4 || scenario || u32(d+pos)!=1)return 0;scenario=1; }
        pos+=length;
    }
    return pos==n && direction && scenario;
}

static int containers(const uint8_t *d,size_t n,uint32_t client,int expanded,size_t *parent)
{
    if(n<4)return 0;
    uint32_t count=u32(d),seen=0,ids[32];size_t pos=4;
    if(count>32)return 0;
    for(uint32_t i=0;i<count;i++) {
        if(!span(pos,8,n))return 0;
        uint32_t id=u32(d+pos),props=u32(d+pos+4);pos+=8;
        for(uint32_t j=0;j<i;j++)if(ids[j]==id)return 0;
        ids[i]=id;
        if(props>64)return 0;
        unsigned parent_seen=0,gc_seen=0;
        for(uint32_t j=0;j<props;j++) {
            if(!span(pos,8,n))return 0;
            uint32_t pid=u32(d+pos),length=u32(d+pos+4);pos+=8;
            if(!span(pos,length,n))return 0;
            if(id==SHIZUKA_RELAY_CONTAINER && pid==PARENT) {
                if(parent_seen++ || length!=4 || u32(d+pos)!=(expanded?client:UINT32_MAX))return 0;
                *parent=pos;
            }
            if(id==SHIZUKA_RELAY_CONTAINER && pid==0x08001014) {
                if(gc_seen++ || length!=4 || u32(d+pos)!=2)return 0;
            }
            pos+=length;
        }
        if(id==SHIZUKA_RELAY_CONTAINER) { if(!parent_seen || !gc_seen)return 0;++seen; }
    }
    return pos==n && seen==1;
}

#if SHIZUKA_RELAY_RAW_ISLAND
/* Both original GC objects already belong to the same positively fresh SG.
 * Require their complete stock property contract before moving any module;
 * never manufacture a new container, property, or namespace reservation.
 */
static int island_containers(const uint8_t *d,size_t n,uint32_t client,int expanded,size_t *encoder_parent)
{
    if(n<4)return 0;
    uint32_t count=u32(d),seen=0;size_t pos=4;
    if(count>32)return 0;
    for(uint32_t i=0;i<count;i++) {
        if(!span(pos,8,n))return 0;
        uint32_t id=u32(d+pos),props=u32(d+pos+4);pos+=8;
        int target=id==SHIZUKA_RELAY_ENCODER_CONTAINER || id==SHIZUKA_RELAY_CONTAINER;
        uint32_t mask=0;
        if(props>64 || (target && props!=7))return 0;
        for(uint32_t j=0;j<props;j++) {
            if(!span(pos,8,n))return 0;
            uint32_t pid=u32(d+pos),length=u32(d+pos+4);pos+=8;
            if(!span(pos,length,n))return 0;
            if(target) {
                uint32_t bit=0,value=0;
                switch(pid) {
                case 0x08001014:bit=1;value=2;break; /* ADSP */
                case 0x08001013:bit=2;value=4096;break;
                case 0x08001011:
                    if(length!=8 || u32(d+pos)!=1 || u32(d+pos+4)!=0x0b001001)return 0;
                    bit=4;break; /* generic container */
                case 0x08001012:bit=8;value=UINT32_MAX;break;
                case PARENT:
                    bit=16;value=expanded?client:UINT32_MAX;
                    if(id==SHIZUKA_RELAY_ENCODER_CONTAINER)*encoder_parent=pos;
                    break;
                case 0x08001174:bit=32;value=1;break;
                case 0x0800124d:bit=64;value=1;break;
                default:return 0;
                }
                if(mask&bit)return 0;
                if(bit!=4 && (length!=4 || u32(d+pos)!=value))return 0;
                mask|=bit;
            }
            pos+=length;
        }
        if(target) {
            uint32_t bit=id==SHIZUKA_RELAY_ENCODER_CONTAINER?1U:2U;
            if(mask!=127 || (seen&bit))return 0;
            seen|=bit;
        }
    }
    return pos==n && seen==3;
}

static unsigned island_node(uint32_t mid,uint32_t iid,int expanded)
{
    if(iid==0x43f3 && mid==0x07001010)return 1;
    if(iid==0x43f4 && mid==0x0700101a)return 2;
    if(iid==0x43f5 && mid==0x07001011)return 4;
    if(iid==0x43f6 && mid==0x07001041)return 8;
    if(iid==SHIZUKA_RELAY_RD && mid==(expanded?0x07001001U:0x0700101aU))return 16;
    if(iid==0x43f8 && mid==0x07001003)return 32;
    if(iid==SHIZUKA_RELAY_WR && mid==(expanded?0x07001000U:0x0700101aU))return 64;
    if(iid==ENCODER && mid==0x07001082)return 128;
    return 0;
}

static int island_modules(const uint8_t *d,size_t n,int expanded,size_t *rd_mid,size_t *wr_mid)
{
    if(n<4)return 0;
    uint32_t groups=u32(d),seen=0,all=0,ids[512],total=0;size_t pos=4;
    if(groups>64)return 0;
    for(uint32_t i=0;i<groups;i++) {
        if(!span(pos,12,n))return 0;
        uint32_t sg=u32(d+pos),cid=u32(d+pos+4),count=u32(d+pos+8);pos+=12;
        if(count>512-total || !span(pos,(size_t)count*8,n))return 0;
        int target=cid==SHIZUKA_RELAY_ENCODER_CONTAINER || cid==SHIZUKA_RELAY_CONTAINER;
        uint32_t mask=0;
        if(target && sg!=SHIZUKA_RELAY_SG)return 0;
        for(uint32_t j=0;j<count;j++) {
            uint32_t mid=u32(d+pos),iid=u32(d+pos+4);
            if(iid<0x4001)return 0;
            for(uint32_t k=0;k<total;k++)if(ids[k]==iid)return 0;
            ids[total++]=iid;
            unsigned bit=island_node(mid,iid,expanded);
            /* The original or transformed module can never be supplied in a
             * foreign SG/container group, even with a different MID. */
            if(iid==0x43f3 || iid==0x43f4 || iid==0x43f5 || iid==0x43f6 ||
                iid==SHIZUKA_RELAY_RD || iid==0x43f8 || iid==SHIZUKA_RELAY_WR || iid==ENCODER) {
                if(!target || !bit || (all&bit))return 0;
                all|=bit;
            }
            if(target) {if(!bit || (mask&bit))return 0;mask|=bit;}
            if(iid==SHIZUKA_RELAY_RD)*rd_mid=pos;
            if(iid==SHIZUKA_RELAY_WR)*wr_mid=pos;
            pos+=8;
        }
        if(target) {
            uint32_t group_bit=cid==SHIZUKA_RELAY_ENCODER_CONTAINER?1U:2U;
            uint32_t expected=cid==SHIZUKA_RELAY_ENCODER_CONTAINER?(expanded?191U:15U):(expanded?64U:240U);
            if(mask!=expected || (seen&group_bit))return 0;
            seen|=group_bit;
        }
    }
    return pos==n && seen==3 && all==255;
}

static int island_connections(const uint8_t *d,size_t n,int expanded)
{
    static const uint32_t common[][4]={
        {0x43f3,1,0x43f4,2},{0x43f4,1,0x43f5,2},
        {0x43f5,1,0x43f6,2},{0x43f6,1,0x43f8,2},
        {0x43f5,3,0x44e9,2}
    };
    if(n<4 || u32(d)!=(expanded?7U:8U))return 0;
    uint32_t seen=0;
    for(uint32_t i=0;i<u32(d);i++) {
        const uint8_t *p=d+4+(size_t)i*16;unsigned matched=0;
        for(unsigned j=0;j<5;j++) {
            unsigned equal=1;
            for(unsigned k=0;k<4;k++)if(u32(p+k*4)!=common[j][k])equal=0;
            if(equal) {if(seen&(1U<<j))return 0;seen|=1U<<j;matched=1;}
        }
        if(!matched) {
            uint32_t a=u32(p),b=u32(p+4),c=u32(p+8),e=u32(p+12);
            if(b!=1 || e!=2)return 0;
            if(expanded) {
                if(!((a==0x43f8 && c==ENCODER) || (a==ENCODER && c==SHIZUKA_RELAY_RD)))return 0;
            } else if(!((a==0x43f8 && c==SHIZUKA_RELAY_RD) ||
                (a==SHIZUKA_RELAY_RD && c==ENCODER) || (a==ENCODER && c==SHIZUKA_RELAY_WR)))return 0;
        }
    }
    return seen==31;
}
#endif

#if !SHIZUKA_RELAY_RAW_ISLAND
static int modules(const uint8_t *d,size_t n,int expanded,size_t *rd_mid,size_t *wr_mid)
{
    if(n<4)return 0;
    uint32_t groups=u32(d),ids[512],total=0,seen=0;size_t pos=4;
    if(groups>64)return 0;
    for(uint32_t i=0;i<groups;i++) {
        if(!span(pos,12,n))return 0;
        uint32_t sg=u32(d+pos),container=u32(d+pos+4),count=u32(d+pos+8);
        pos+=12;
        if(count>512-total || !span(pos,(size_t)count*8,n))return 0;
        unsigned mask=0;
        for(uint32_t j=0;j<count;j++) {
            uint32_t mid=u32(d+pos),iid=u32(d+pos+4);
            if(iid<0x4001)return 0;
            for(uint32_t k=0;k<total;k++)if(ids[k]==iid)return 0;
            ids[total++]=iid;
            if((iid==SHIZUKA_RELAY_RD || iid==SHIZUKA_RELAY_WR)
               && !(sg==SHIZUKA_RELAY_SG && container==SHIZUKA_RELAY_CONTAINER))return 0;
            if(sg==SHIZUKA_RELAY_SG && container==SHIZUKA_RELAY_CONTAINER) {
                unsigned bit=0;
                if(iid==SHIZUKA_RELAY_RD && mid==(expanded?0x07001001U:0x0700101aU)) {bit=1;*rd_mid=pos;}
                if(iid==0x43f8 && mid==0x07001003)bit=2;
                if(iid==SHIZUKA_RELAY_WR && mid==(expanded?0x07001000U:0x0700101aU)) {bit=4;*wr_mid=pos;}
                if(iid==ENCODER && mid==0x07001082)bit=8;
                if(!bit || (mask&bit))return 0;
                mask|=bit;
            }
            pos+=8;
        }
        if(sg==SHIZUKA_RELAY_SG && container==SHIZUKA_RELAY_CONTAINER) {
            if(count!=4 || mask!=15)return 0;
            ++seen;
        }
    }
    return pos==n && seen==1;
}
#endif

static int properties(const uint8_t *d,size_t n,int expanded,size_t *rd_ports,size_t *wr_ports)
{
    if(n<4)return 0;
    uint32_t count=u32(d),ids[512];size_t pos=4;unsigned seen=0;
    if(count>512)return 0;
    for(uint32_t i=0;i<count;i++) {
        if(!span(pos,8,n))return 0;
        uint32_t iid=u32(d+pos),props=u32(d+pos+4);pos+=8;
        for(uint32_t j=0;j<i;j++)if(ids[j]==iid)return 0;
        ids[i]=iid;if(props>64)return 0;
        unsigned required=0,port_seen=0;uint32_t inputs=1,outputs=1;
        if(iid==ENCODER)required=1;
        if(iid==SHIZUKA_RELAY_RD) { required=2;if(expanded)outputs=0; }
        if(iid==SHIZUKA_RELAY_WR) { required=4;if(expanded)inputs=0; }
        for(uint32_t j=0;j<props;j++) {
            if(!span(pos,8,n))return 0;
            uint32_t pid=u32(d+pos),length=u32(d+pos+4);pos+=8;
            if(!span(pos,length,n))return 0;
            if(required && pid==PORTS) {
                if(port_seen++ || length!=8 || u32(d+pos)!=inputs || u32(d+pos+4)!=outputs)return 0;
                if(iid==SHIZUKA_RELAY_RD)*rd_ports=pos;
                if(iid==SHIZUKA_RELAY_WR)*wr_ports=pos;
            }
            pos+=length;
        }
        if(required) { if(!port_seen)return 0;seen|=required; }
    }
    return pos==n && seen==7;
}

#if SHIZUKA_RELAY_RAW_ISLAND
static int island_properties(const uint8_t *d,size_t n,int expanded)
{
    if(n<4)return 0;
    size_t pos=4;uint32_t seen=0;
    for(uint32_t i=0;i<u32(d);i++) {
        if(!span(pos,8,n))return 0;
        uint32_t iid=u32(d+pos),count=u32(d+pos+4);pos+=8;
        unsigned bit=0;uint32_t inputs=1,outputs=1;
        switch(iid) {
        case 0x43f3:bit=1;inputs=5;break;
        case 0x43f4:bit=2;break;
        case 0x43f5:bit=4;outputs=3;break;
        case 0x43f6:bit=8;break;
        case SHIZUKA_RELAY_RD:bit=16;if(expanded)outputs=0;break;
        case 0x43f8:bit=32;break;
        case SHIZUKA_RELAY_WR:bit=64;if(expanded)inputs=0;break;
        case ENCODER:bit=128;break;
        default:break;
        }
        unsigned ports=0;
        for(uint32_t j=0;j<count;j++) {
            if(!span(pos,8,n))return 0;
            uint32_t pid=u32(d+pos),length=u32(d+pos+4);pos+=8;
            if(!span(pos,length,n))return 0;
            if(bit && pid==PORTS) {
                if(ports++ || length!=8 || u32(d+pos)!=inputs || u32(d+pos+4)!=outputs)return 0;
            }
            pos+=length;
        }
        if(bit) {if(!ports || (seen&bit))return 0;seen|=bit;}
    }
    return pos==n && seen==255;
}
#endif

static int connections(const uint8_t *d,size_t n,int expanded)
{
    if(n<4)return 0;
    uint32_t count=u32(d),pcm_in=0,pcm_out=0,encoded=0,direct_pcm=0,rd=0;
    if(count>256 || n!=4+(size_t)count*16)return 0;
    for(uint32_t i=0;i<count;i++) {
        size_t pos=4+(size_t)i*16;uint32_t a=u32(d+pos),b=u32(d+pos+4),c=u32(d+pos+8),e=u32(d+pos+12);
        if(!b || b>=0x80000000 || !(b&1) || !e || e>=0x80000000 || (e&1))return 0;
        for(uint32_t j=0;j<i;j++) {
            const uint8_t *p=d+4+(size_t)j*16;
            if((a==u32(p) && b==u32(p+4)) || (c==u32(p+8) && e==u32(p+12)))return 0;
        }
        if(a==0x43f8 && b==1 && c==SHIZUKA_RELAY_RD && e==2)++pcm_in;
        if(a==SHIZUKA_RELAY_RD && b==1 && c==ENCODER && e==2)++pcm_out;
        if(a==ENCODER && b==1 && c==SHIZUKA_RELAY_WR && e==2)++encoded;
        if(a==0x43f8 && b==1 && c==ENCODER && e==2)++direct_pcm;
        if(a==ENCODER && b==1 && c==SHIZUKA_RELAY_RD && e==2)++rd;
        if(expanded && (a==SHIZUKA_RELAY_RD || c==SHIZUKA_RELAY_WR
            || (c==SHIZUKA_RELAY_RD && !(a==ENCODER && b==1 && e==2))
            || (a==SHIZUKA_RELAY_WR && !(b==1 && c==0x4098 && e==2))))return 0;
    }
    return expanded?(!pcm_in && !pcm_out && !encoded && direct_pcm==1 && rd==1)
                   :(pcm_in==1 && pcm_out==1 && encoded==1 && !direct_pcm && !rd);
}

static int controls(const uint8_t *d,size_t n,unsigned *encoder_links)
{
    if(n<4)return 0;
    uint32_t count=u32(d);size_t pos=4;if(count>256)return 0;
    for(uint32_t i=0;i<count;i++) {
        if(!span(pos,20,n))return 0;
        uint32_t peer1=u32(d+pos),port1=u32(d+pos+4),peer2=u32(d+pos+8),port2=u32(d+pos+12),props=u32(d+pos+16);
        if(peer1==0x4edd && port1==0xc0000001 && peer2==ENCODER && port2==0xc0000001)++*encoder_links;
        pos+=20;if(props>64)return 0;
        for(uint32_t j=0;j<props;j++) {
            if(!span(pos,8,n))return 0;
            uint32_t length=u32(d+pos+4);pos+=8;
            if(!span(pos,length,n))return 0;pos+=length;
        }
    }
    return pos==n;
}

static int parse(const uint8_t *in,size_t size,uint32_t client,int expanded,struct plan *plan)
{
    if(!in || size<16 || size>SHIZUKA_RELAY_MAX_SPF || !client_valid(client))return SHIZUKA_GRAPH_INVALID;
    plan->end=plan->parent=plan->encoder_parent=plan->modules=plan->rd_mid=plan->wr_mid=ABSENT;
    plan->properties=plan->connections=plan->rd_ports=plan->wr_ports=ABSENT;
    size_t pos=0;uint32_t current=0;unsigned sg_count=0,seen=0,encoder_links=0,added=0,params=0;
    while(pos<size) {
        struct parameter p;if(!next(in,size,&pos,&p) || ++params>4096)return SHIZUKA_GRAPH_INVALID;
        if(p.miid==1 && p.pid==SG_CFG) {
            if(p.size<8 || u32(in+p.data)!=1)return SHIZUKA_GRAPH_INVALID;
            if(current==SHIZUKA_RELAY_SG)plan->end=p.start;
            current=u32(in+p.data+4);
            if(current==SHIZUKA_RELAY_SG) {
                if(++sg_count!=1 || !sg_properties(in+p.data,p.size))return SHIZUKA_GRAPH_INVALID;
            }
        }
        if(current!=SHIZUKA_RELAY_SG) {
            if(p.miid==SHIZUKA_RELAY_RD || p.miid==SHIZUKA_RELAY_WR)return SHIZUKA_GRAPH_INVALID;
            continue;
        }
        const uint8_t *d=in+p.data;unsigned bit=0;
        if(p.miid==1) {
            switch(p.pid) {
            case CONTAINER_CFG: {
                size_t parent=0;bit=1;
                if(!containers(d,p.size,client,expanded,&parent))return SHIZUKA_GRAPH_INVALID;
                plan->parent=p.data+parent;
#if SHIZUKA_RELAY_RAW_ISLAND
                if(!island_containers(d,p.size,client,expanded,&parent))return SHIZUKA_GRAPH_INVALID;
                plan->encoder_parent=p.data+parent;
#endif
                break;
            }
            case MODULES: {
                size_t rd=0,wr=0;bit=2;
#if SHIZUKA_RELAY_RAW_ISLAND
                if(!island_modules(d,p.size,expanded,&rd,&wr))return SHIZUKA_GRAPH_INVALID;
#else
                if(!modules(d,p.size,expanded,&rd,&wr))return SHIZUKA_GRAPH_INVALID;
#endif
                plan->modules=p.start;plan->rd_mid=p.data+rd;plan->wr_mid=p.data+wr;break;
            }
            case PROPS: {
                size_t rd=0,wr=0;bit=4;
                if(!properties(d,p.size,expanded,&rd,&wr))return SHIZUKA_GRAPH_INVALID;
#if SHIZUKA_RELAY_RAW_ISLAND
                if(!island_properties(d,p.size,expanded))return SHIZUKA_GRAPH_INVALID;
#endif
                plan->properties=p.start;plan->rd_ports=p.data+rd;plan->wr_ports=p.data+wr;break;
            }
            case CONNS: {
                bit=8;
                if(!connections(d,p.size,expanded))return SHIZUKA_GRAPH_INVALID;
#if SHIZUKA_RELAY_RAW_ISLAND
                if(!island_connections(d,p.size,expanded))return SHIZUKA_GRAPH_INVALID;
#endif
                plan->connections=p.start;break;
            }
            case CTRL: if(!controls(d,p.size,&encoder_links))return SHIZUKA_GRAPH_INVALID;break;
            default:break;
            }
            if(bit && (seen&bit))return SHIZUKA_GRAPH_INVALID;
            seen|=bit;
        }
        if(p.miid==SHIZUKA_RELAY_RD || p.miid==SHIZUKA_RELAY_WR) {
            if(!expanded || p.size!=8)return SHIZUKA_GRAPH_INVALID;
            unsigned flag=0;
            if(p.miid==SHIZUKA_RELAY_RD && p.pid==0x0800114d && u32(d)==1 && u32(d+4)==client)flag=1;
            if(p.miid==SHIZUKA_RELAY_WR && p.pid==0x080010c0 && u32(d)==1 && u32(d+4)==client)flag=2;
            if(p.miid==SHIZUKA_RELAY_RD && p.pid==0x08001007 && u32(d)==1 && u32(d+4)==1)flag=4;
            if(!flag || (added&flag))return SHIZUKA_GRAPH_INVALID;
            added|=flag;
        }
    }
    if(current==SHIZUKA_RELAY_SG)plan->end=size;
    if(sg_count!=1 || seen!=15 || encoder_links!=1 || plan->end==ABSENT || (expanded && added!=7))return SHIZUKA_GRAPH_INVALID;
    return SHIZUKA_GRAPH_OK;
}

int shizuka_relay_graph_measure(const uint8_t *in,size_t size,uint32_t client,size_t *output_size)
{
    if(!output_size)return SHIZUKA_GRAPH_INVALID;
    *output_size=0;struct plan plan;int result=parse(in,size,client,0,&plan);
    if(result)return result;
    if(size>SHIZUKA_RELAY_MAX_SPF-SHIZUKA_RELAY_GRAPH_GROWTH)return SHIZUKA_GRAPH_CAPACITY;
    *output_size=size+SHIZUKA_RELAY_GRAPH_GROWTH;return SHIZUKA_GRAPH_OK;
}

int shizuka_relay_graph_verify(const uint8_t *in,size_t size,uint32_t client)
{ struct plan plan;return parse(in,size,client,1,&plan); }

static void words(uint8_t *out,size_t *pos,const uint32_t *values,size_t n)
{ for(size_t i=0;i<n;i++){put32(out+*pos,values[i]);*pos+=4;} }

#if SHIZUKA_RELAY_RAW_ISLAND
static int island_write_modules(uint8_t *out,const uint8_t *d,size_t n)
{
    /* island_modules has already validated every group and all eight owned
     * nodes. Counts and group payload totals change by opposite24B amounts;
     * the containing parameter size and its padding remain exactly unchanged.
     */
    const uint8_t *original_e6=0;size_t pos=4,w=4;
    for(uint32_t i=0;i<u32(d);i++) {
        uint32_t count=u32(d+pos+8);
        if(u32(d+pos)==SHIZUKA_RELAY_SG && u32(d+pos+4)==SHIZUKA_RELAY_CONTAINER)original_e6=d+pos+12;
        pos+=12+(size_t)count*8;
    }
    if(pos!=n || !original_e6)return 0;
    put32(out,u32(d));pos=4;
    for(uint32_t i=0;i<u32(d);i++) {
        uint32_t sg=u32(d+pos),cid=u32(d+pos+4),count=u32(d+pos+8);
        const uint8_t *pairs=d+pos+12;
        uint32_t header[]={sg,cid,count};
        if(sg==SHIZUKA_RELAY_SG && cid==SHIZUKA_RELAY_ENCODER_CONTAINER)header[2]=7;
        if(sg==SHIZUKA_RELAY_SG && cid==SHIZUKA_RELAY_CONTAINER)header[2]=1;
        words(out,&w,header,3);
        if(sg==SHIZUKA_RELAY_SG && cid==SHIZUKA_RELAY_ENCODER_CONTAINER) {
            copy(out+w,pairs,(size_t)count*8);w+=(size_t)count*8;
            for(unsigned j=0;j<4;j++) {
                uint32_t mid=u32(original_e6+j*8),iid=u32(original_e6+j*8+4);
                if(iid==SHIZUKA_RELAY_WR)continue;
                if(iid==SHIZUKA_RELAY_RD)mid=0x07001001;
                uint32_t pair[]={mid,iid};words(out,&w,pair,2);
            }
        } else if(sg==SHIZUKA_RELAY_SG && cid==SHIZUKA_RELAY_CONTAINER) {
            uint32_t pair[]={0x07001000,SHIZUKA_RELAY_WR};words(out,&w,pair,2);
        } else {copy(out+w,pairs,(size_t)count*8);w+=(size_t)count*8;}
        pos+=12+(size_t)count*8;
    }
    return w==n;
}
#endif

int shizuka_relay_graph_transform(const uint8_t *in,size_t size,uint32_t client,uint8_t *out,size_t capacity,size_t *output_size)
{
    if(!output_size)return SHIZUKA_GRAPH_INVALID;
    *output_size=0;struct plan plan;int result=parse(in,size,client,0,&plan);
    if(result)return result;
    if(!out || size>SHIZUKA_RELAY_MAX_SPF-SHIZUKA_RELAY_GRAPH_GROWTH || capacity<size+SHIZUKA_RELAY_GRAPH_GROWTH)return SHIZUKA_GRAPH_CAPACITY;
    uintptr_t a=(uintptr_t)in,b=(uintptr_t)out;
    if(a>UINTPTR_MAX-size || b>UINTPTR_MAX-(size+SHIZUKA_RELAY_GRAPH_GROWTH))return SHIZUKA_GRAPH_INVALID;
    if(a<b+size+SHIZUKA_RELAY_GRAPH_GROWTH && b<a+size)return SHIZUKA_GRAPH_OVERLAP;
    size_t pos=0,w=0;
    while(pos<size) {
        struct parameter p;if(!next(in,size,&pos,&p))return SHIZUKA_GRAPH_INVALID;
        size_t start=w;copy(out+w,in+p.start,16);w+=16;
        if(p.start==plan.modules) {
#if SHIZUKA_RELAY_RAW_ISLAND
            if(!island_write_modules(out+w,in+p.data,p.size))return SHIZUKA_GRAPH_INVALID;
            w+=p.size;
            copy(out+w,in+p.data+p.size,p.total-16-p.size);w+=p.total-16-p.size;
#else
            copy(out+w,in+p.data,p.total-16);w+=p.total-16;
            put32(out+start+16+plan.rd_mid-p.data,0x07001001);
            put32(out+start+16+plan.wr_mid-p.data,0x07001000);
#endif
        } else if(p.start==plan.properties) {
            copy(out+w,in+p.data,p.total-16);w+=p.total-16;
            put32(out+start+16+plan.rd_ports-p.data+4,0);
            put32(out+start+16+plan.wr_ports-p.data,0);
        } else if(p.start==plan.connections) {
            put32(out+start+8,(uint32_t)p.size-16);
            put32(out+w,u32(in+p.data)-1);w+=4;
            for(size_t edge=4;edge<p.size;edge+=16) {
                const uint8_t *old=in+p.data+edge;
                uint32_t a=u32(old),b=u32(old+4),c=u32(old+8),d=u32(old+12);
                if(a==SHIZUKA_RELAY_RD && b==1 && c==ENCODER && d==2)continue;
                if(a==0x43f8 && b==1 && c==SHIZUKA_RELAY_RD && d==2)c=ENCODER;
                else if(a==ENCODER && b==1 && c==SHIZUKA_RELAY_WR && d==2)c=SHIZUKA_RELAY_RD;
                uint32_t edge_words[]={a,b,c,d};words(out,&w,edge_words,4);
            }
            copy(out+w,in+p.data+p.size,p.total-16-p.size);w+=p.total-16-p.size;
        } else {
            copy(out+w,in+p.data,p.total-16);w+=p.total-16;
            if(plan.parent>=p.data && plan.parent+4<=p.data+p.size)put32(out+start+16+plan.parent-p.data,client);
#if SHIZUKA_RELAY_RAW_ISLAND
            if(plan.encoder_parent>=p.data && plan.encoder_parent+4<=p.data+p.size)put32(out+start+16+plan.encoder_parent-p.data,client);
#endif
        }
        if(pos==plan.end) {
            uint32_t params[]={SHIZUKA_RELAY_RD,0x0800114d,8,0,1,client,
                SHIZUKA_RELAY_WR,0x080010c0,8,0,1,client,
                SHIZUKA_RELAY_RD,0x08001007,8,0,1,1};words(out,&w,params,18);
        }
    }
    if(w!=size+SHIZUKA_RELAY_GRAPH_GROWTH || shizuka_relay_graph_verify(out,w,client))return SHIZUKA_GRAPH_INVALID;
    *output_size=w;return SHIZUKA_GRAPH_OK;
}
