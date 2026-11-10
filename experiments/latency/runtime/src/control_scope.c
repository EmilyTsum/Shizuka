/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/control_scope.h"

static uint32_t get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int packet_valid(const void *packet,size_t capacity,uint32_t *packet_size)
{
    const uint8_t *p=packet;
    if(!p || capacity<40 || (uintptr_t)p>UINTPTR_MAX-capacity)return 0;
    uint32_t header=get32(p),bytes=header>>8,opcode=get32(p+20);
    if((header&255u)!=0x60 || bytes<40 || bytes>capacity || bytes>SHIZUKA_CONTROL_MAX_PACKET_BYTES
        || p[4]!=2 || p[5]!=3 || p[7] || !get32(p+8) || get32(p+12)!=1
        || opcode<0x01001001 || opcode>0x01001004)return 0;
    *packet_size=bytes;return 1;
}
static int parse_body(const uint8_t *p,size_t bytes,uint32_t target_sg)
{
    size_t at=0;uint32_t lists=0,records=0;int target=0;
    while(at<bytes) {
        if(bytes-at<16 || ++records>4096)return -1;
        uint32_t mid=get32(p+at),pid=get32(p+at+4),size=get32(p+at+8);
        size_t available=bytes-at-16;
        if(get32(p+at+12) || size>available)return -1;
        size_t padded=((size_t)size+7)&~(size_t)7;
        if(padded>available)return -1;
        if(pid==0x08001005) {
            if(mid!=1 || ++lists!=1 || size<4)return -1;
            uint32_t count=get32(p+at+16);
            if(count>SHIZUKA_CONTROL_MAX_SUBGRAPHS || count!=(size-4)/4 || size%4)return -1;
            for(uint32_t i=0;i<count;i++) {
                uint32_t id=get32(p+at+20+(size_t)i*4);
                if(!id)return -1;
                for(uint32_t j=0;j<i;j++)
                    if(id==get32(p+at+20+(size_t)j*4))return -1;
                if(id==target_sg)target=1;
            }
        }
        at+=16+padded;
    }
    return target;
}
int shizuka_control_scope_target(const void *packet,size_t capacity)
{
    const uint8_t *p=packet;uint32_t bytes;
    if(!packet_valid(packet,capacity,&bytes))return -1;
    /* fwk/api/apm/apm_api.h: payload address LSW/MSW, map handle, payload size.
     * Never follow an out-of-band address, even when an inband tail is present.
     */
    if(get32(p+24) || get32(p+28) || get32(p+32) || get32(p+36)!=bytes-40)return -1;
    return parse_body(p+40,bytes-40,SHIZUKA_CONTROL_TARGET_SG);
}
int shizuka_control_scope_target_owned_body(const void *packet,size_t packet_capacity,
    const void *body,size_t body_capacity)
{
    const uint8_t *p=packet;uint32_t bytes;
    if(!packet_valid(packet,packet_capacity,&bytes) || bytes!=40 || !get32(p+32)
        || !body || !body_capacity || body_capacity>SHIZUKA_CONTROL_MAX_PACKET_BYTES
        || (uintptr_t)body>UINTPTR_MAX-body_capacity || get32(p+36)>body_capacity)return -1;
    uintptr_t a=(uintptr_t)packet,b=(uintptr_t)body;
    if(a<b+body_capacity && b<a+packet_capacity)return -1;
    /* Address/map ownership was attested by the caller; only this independent
     * live CPU view is accessed. In particular ADSP address zero can be valid
     * with a nonzero map handle and must never be cast into a CPU pointer.
     */
    return parse_body(body,get32(p+36),SHIZUKA_CONTROL_TARGET_SG);
}

int shizuka_control_scope_start_peer_present(const void *packet,size_t capacity)
{
    const uint8_t *p=packet;uint32_t bytes;
    if(!packet_valid(packet,capacity,&bytes) || get32(p+20)!=0x01001002u ||
        get32(p+24) || get32(p+28) || get32(p+32) || get32(p+36)!=bytes-40)return -1;
    return parse_body(p+40,bytes-40,UINT32_C(0xb000006c));
}
