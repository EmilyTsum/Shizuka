/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/encoded_transform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
static void check(int good) { ++checks; if (!good) { fprintf(stderr,"C transform assertion %lu failed\n",checks); exit(1); } }
static void put32(unsigned char *p,unsigned value) { unsigned i; for(i=0;i<4;++i) p[i]=(unsigned char)(value>>(8*i)); }
static unsigned get16(const unsigned char *p) { return p[0] | ((unsigned)p[1]<<8); }
static const unsigned payloads[9]={348,656,140,152,560,760,960,348,980};
static unsigned packet(unsigned char *p,unsigned units,unsigned kind,unsigned role,unsigned marker) {
    unsigned i; p[0]=(unsigned char)units; p[1]=(unsigned char)(units>>8); p[2]=120; p[3]=(unsigned char)kind;
    p[4]=(unsigned char)role; p[5]=p[6]=0; p[7]=(unsigned char)marker;
    for(i=0;i<payloads[kind];++i) p[8+i]=(unsigned char)(i*17+3);
    return payloads[kind]+8;
}
static unsigned md_record(unsigned char *p,unsigned id,unsigned bytes,unsigned value) {
    memset(p,0,24+bytes); put32(p,id); put32(p+4,0x21); put32(p+20,bytes);
    put32(p+24,value); return 24+bytes;
}
static unsigned metadata(unsigned char *p,unsigned bytes,unsigned frame_position) {
    unsigned pos=0,i;
    for(i=0;i<4;++i) {
        if(i==frame_position) {
            pos+=md_record(p+pos,0x0a001052,16,bytes); put32(p+pos-36,0);
            put32(p+pos-12,0xdcba9877); put32(p+pos-8,0xfffffffe); put32(p+pos-4,0x80000000);
        } else if(i<(frame_position?frame_position:1)) pos+=md_record(p+pos,0x0a001037,4,1440);
        else pos+=md_record(p+pos,0x0a001039,4,0);
    }
    return pos;
}
static int run(unsigned char *data,unsigned bytes,unsigned char *md,unsigned mdbytes,unsigned long long delta,
               struct shz_encoded_result *result) {
    return shz_encoded_transform(data,bytes,4096,md,mdbytes,256,delta,1,result);
}
int main(void) {
    unsigned char data[4096], original[4096], md[256], oldmd[256];
    struct shz_encoded_result result, old_result;
    unsigned units,di,type,ri,marker,bytes,mdbytes,i,position;
    const unsigned deltas[4]={0,50000,100000,150000},roles[4]={0,0x80,0xa0,0xc0};
    for(di=0;di<4;++di) for(units=0;units<65536;++units) {
        int status;
        bytes=packet(data,units,2,0xa0,0xae); mdbytes=metadata(md,bytes,3);
        memcpy(original,data,bytes); memcpy(oldmd,md,mdbytes);
        memset(&result,0x5a,sizeof(result)); old_result=result;
        struct shz_encoded_result probe;
        memset(&probe,0x5a,sizeof(probe));
        int probed=shz_encoded_validate(data,bytes,4096,md,mdbytes,256,deltas[di],1,&probe);
        check(!memcmp(data,original,bytes)&&!memcmp(md,oldmd,mdbytes));
        status=run(data,bytes,md,mdbytes,deltas[di],&result);
        check(probed==status&&!memcmp(&probe,&result,sizeof(result)));
        if((!units && deltas[di]) || units+deltas[di]/8>65535) {
            check(status!=0 && !memcmp(data,original,bytes) && !memcmp(md,oldmd,mdbytes) &&
                  !memcmp(&result,&old_result,sizeof(result)));
        } else {
            check(status==0 && get16(data)==units+deltas[di]/8 && !memcmp(data+2,original+2,bytes-2));
            check(result.timestamp_bits==0xfffffffedcba9877ULL && result.wr_flags==0x80000010U);
            check(result.wrapper_count==1 && result.metadata_bytes==mdbytes-40);
        }
    }
    for(type=0;type<9;++type) for(ri=0;ri<4;++ri) for(marker=0xad;marker<=0xaf;++marker) {
        bytes=packet(data,roles[ri]==0xc0?0:24625,type,roles[ri],marker); mdbytes=metadata(md,bytes,1);
        memcpy(original,data,bytes); check(run(data,bytes,md,mdbytes,150000,&result)==0);
        check(get16(data)==(roles[ri]==0xc0?0:43375) && !memcmp(data+2,original+2,bytes-2));
        check(result.secondary_zero_count==(roles[ri]==0xc0));
    }
    for(position=0;position<4;++position) {
        unsigned consumed=0,out=0;
        bytes=packet(data,24625,1,0xa0,0xae); bytes+=packet(data+bytes,0,1,0xc0,0xae);
        mdbytes=metadata(md,bytes,position); memcpy(oldmd,md,mdbytes);
        check(run(data,bytes,md,mdbytes,150000,&result)==0);
        for(i=0;i<mdbytes;) {
            unsigned size=oldmd[i+20]+24;
            if(oldmd[i]==0x52) consumed+=size;
            else { check(!memcmp(md+out,oldmd+i,size)); out+=size; }
            i+=size;
        }
        check(consumed==40 && result.metadata_bytes==out && result.wrapper_count==2 &&
              result.changed_wrapper_count==1 && result.secondary_zero_count==1 &&
              result.first_ttp_before_us==197000 && result.first_ttp_after_us==347000);
    }
    /* A late overflowing primary must not change the earlier primary or MD. */
    bytes=packet(data,24625,1,0,0xae); bytes+=packet(data+bytes,50000,1,0,0xae);
    mdbytes=metadata(md,bytes,3); memcpy(original,data,bytes); memcpy(oldmd,md,mdbytes);
    check(run(data,bytes,md,mdbytes,150000,&result)==SHZ_ENCODED_TTP_OVERFLOW);
    check(!memcmp(data,original,bytes) && !memcmp(md,oldmd,mdbytes));
    check(shz_encoded_transform(data,bytes,bytes-1,md,mdbytes,256,0,1,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,md,mdbytes,mdbytes-1,0,1,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,data,mdbytes,256,0,1,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,md,mdbytes,256,0,2,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,md,mdbytes,256,UINT64_MAX,1,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,md,mdbytes,256,7,1,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,md,mdbytes,256,0,1,(struct shz_encoded_result *)(void *)data)!=0);
    check(shz_encoded_transform(data,SHZ_ENCODED_MAX_DATA+1,UINT32_MAX,md,mdbytes,256,0,1,&result)!=0);
    check(shz_encoded_transform(data,bytes,4096,md,SHZ_ENCODED_MAX_METADATA+1,UINT32_MAX,0,1,&result)!=0);

    for(i=0;i<30000;++i) {
        unsigned mutate=(i*1103515245U+12345U), at;
        int status;
        bytes=packet(data,24625,1,0xa0,0xae); bytes+=packet(data+bytes,0,1,0xc0,0xae);
        mdbytes=metadata(md,bytes,i%4);
        if(i%3==0) { at=mutate%bytes; data[at]^=(unsigned char)(1U<<(i%8)); }
        else if(i%3==1) { at=mutate%mdbytes; md[at]^=(unsigned char)(1U<<(i%8)); }
        else { if(i%2) bytes=mutate%bytes; else mdbytes=mutate%mdbytes; }
        memcpy(original,data,bytes); memcpy(oldmd,md,mdbytes); memset(&result,0x5a,sizeof(result)); old_result=result;
        status=run(data,bytes,md,mdbytes,150000,&result);
        if(status) check(!memcmp(data,original,bytes) && !memcmp(md,oldmd,mdbytes) && !memcmp(&result,&old_result,sizeof(result)));
        else {
            unsigned cursor=0;
            while(cursor<bytes) { unsigned n=payloads[data[cursor+3]]+8; check(!memcmp(data+cursor+2,original+cursor+2,n-2)); cursor+=n; }
            check(result.data_bytes==bytes && result.metadata_bytes+40==mdbytes);
        }
    }
    printf("%lu C transform checks passed; 262144 arithmetic,108 formats,30000 mutation cases\n",checks);
    return 0;
}
