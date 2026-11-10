#!/usr/bin/env python3
"""Construct an offline GSL runtime candidate from the single reviewed stock ELF."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
from elftools.elf.elffile import ELFFile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gsl_hook'))
from patch_gsl import STOCK_SHA256,PH,SH,align,branch,raw_headers,vaddr_offset,require

def patch(stock,payload_raw):
    require(hashlib.sha256(stock).hexdigest()==STOCK_SHA256,'wrong stock GSL')
    old=ELFFile(io.BytesIO(stock));payload=ELFFile(io.BytesIO(payload_raw))
    require(payload['e_machine']=='EM_AARCH64' and payload.little_endian,'wrong payload')
    symbols={s.name:s['st_value'] for s in payload.get_section_by_name('.symtab').iter_symbols() if s.name}
    segments=[s for s in payload.iter_segments() if s['p_type']=='PT_LOAD' and s['p_memsz']]
    require(len(segments)==2 and segments[0]['p_flags']==5 and segments[1]['p_flags']==6,'expected separate RX/RW')
    require(segments[0]['p_vaddr']==0x48000 and segments[1]['p_vaddr']%0x4000==0,'payload alignment')
    require(segments[0]['p_memsz']==segments[0]['p_filesz'],'RX BSS is forbidden')
    require(all(s['p_filesz']<=s['p_memsz']<=8*1024*1024 for s in segments),'payload bounds')
    headers=raw_headers(stock);metadata_off=align(len(stock));count=len(headers)+3;table_size=count*PH.size
    require(align(max(h[3]+h[6] for h in headers if h[0]==1))<=0x44000,'stock mapping overlaps metadata')
    for h in headers:
        if h[0]==6:h[:]=[6,4,metadata_off,0x44000,0x44000,table_size,table_size,8]
    headers.append([1,4,metadata_off,0x44000,0x44000,table_size,table_size,0x4000])
    raw=bytearray(stock);raw.extend(bytes(metadata_off+0x4000-len(raw)))
    added=[]
    for i,s in enumerate(segments):
        off=align(len(raw));raw.extend(bytes(off-len(raw)));raw.extend(s.data())
        headers.append([1,s['p_flags'],off,s['p_vaddr'],s['p_vaddr'],s['p_filesz'],s['p_memsz'],0x4000])
        added.append((b'.shizuka.text\0' if i==0 else b'.shizuka.data\0',s,off))
    raw[metadata_off:metadata_off+table_size]=b''.join(PH.pack(*h) for h in headers)
    rel=old.get_section_by_name('.rela.plt');ds=old.get_section(rel['sh_link']);plt=old.get_section_by_name('.plt')
    wanted={'acdb_ioctl':'acdb','__gpr_cmd_register':'register','__gpr_cmd_deregister':'deregister','__gpr_cmd_async_send':'send',
            'gsl_send_spf_cmd_wait_for_basic_rsp':'basic','gpr_deinit':'deinit','gsl_signal_set':'signal',
            'gsl_msg_alloc_ext':'message_alloc','gsl_msg_free':'message_free'}
    edits=[]
    for i,r in enumerate(rel.iter_relocations()):
        name=ds.get_symbol(r['r_info_sym']).name
        if name not in wanted:continue
        entry=plt['sh_addr']+32+i*24;address=entry+12;off=vaddr_offset(old,address,4)
        require(stock[off:off+4]==bytes.fromhex('20021fd6'),'unexpected PLT final branch')
        target=symbols['shizuka_runtime_'+wanted[name]+'_gateway'];raw[off:off+4]=branch(address,target)
        edits.append({'name':name,'rva':address,'target':target,'offset':off})
    require(len(edits)==len(wanted),'missing hook import')
    cfi=vaddr_offset(old,0x19004,4)
    require(struct.unpack_from('<I',stock,cfi)[0]==0xd29e3828,'unexpected original CFI first instruction')
    raw[cfi:cfi+4]=branch(0x19004,symbols['shizuka_runtime_cfi_gateway'])
    edits.append({'name':'exact callback CFI extension','rva':0x19004,'target':symbols['shizuka_runtime_cfi_gateway'],'offset':cfi})
    shs=[list(SH.unpack_from(stock,old['e_shoff']+i*SH.size)) for i in range(old['e_shnum'])]
    names=bytearray(old.get_section(old['e_shstrndx']).data());extra=[]
    name_at=len(names);names.extend(b'.shizuka.phdr\0');extra.append([name_at,1,2,0x44000,metadata_off,table_size,0,0,8,0])
    for name,s,off in added:
        name_at=len(names);names.extend(name)
        extra.append([name_at,1 if s['p_filesz'] else 8,(1 if s['p_flags']&2 else 0)|2|(4 if s['p_flags']&1 else 0),
                      s['p_vaddr'],off,s['p_memsz'],0,0,64,0])
    stringoff=align(len(raw),8);raw.extend(bytes(stringoff-len(raw)));raw.extend(names)
    shs[old['e_shstrndx']][4:6]=[stringoff,len(names)]
    shoff=align(len(raw),8);raw.extend(bytes(shoff-len(raw)));raw.extend(b''.join(SH.pack(*h) for h in shs+extra))
    struct.pack_into('<Q',raw,0x20,metadata_off);struct.pack_into('<Q',raw,0x28,shoff)
    struct.pack_into('<H',raw,0x38,count);struct.pack_into('<H',raw,0x3c,len(shs)+len(extra))
    allowed=set(range(0x20,0x30))|set(range(0x38,0x3a))|set(range(0x3c,0x3e))
    for edit in edits:allowed.update(range(edit['offset'],edit['offset']+4))
    require(all(a==b or i in allowed for i,(a,b) in enumerate(zip(stock,raw))),'unlisted original-byte edit')
    new=ELFFile(io.BytesIO(raw));loads=[s for s in new.iter_segments() if s['p_type']=='PT_LOAD']
    ranges=sorted((s['p_vaddr']//0x4000*0x4000,align(s['p_vaddr']+s['p_memsz'])) for s in loads)
    require(all(a[1]<=b[0] for a,b in zip(ranges,ranges[1:])),'overlapping LOAD pages')
    require(all(s['p_flags']!=7 and s['p_filesz']<=s['p_memsz'] and s['p_offset']+s['p_filesz']<=len(raw) for s in loads),'bad LOAD bounds')
    require(new.get_section_by_name('.dynamic').data()==old.get_section_by_name('.dynamic').data(),'dynamic dependencies changed')
    return bytes(raw),{'stock_sha256':STOCK_SHA256,'candidate_sha256':hashlib.sha256(raw).hexdigest(),
        'payload_sha256':hashlib.sha256(payload_raw).hexdigest(),'edits':edits,'program_headers':count,
        'observer_only':not bool(symbols['shizuka_runtime_relay_mode']),
        'relay_enabled':bool(symbols['shizuka_runtime_relay_mode']),
        'new_gpr_transport':False,'loaded_on_device':False}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ['stock','payload','output']:p.add_argument('--'+key,type=Path,required=True)
    a=p.parse_args();out=a.output.resolve()
    if any((v/'.git').exists() for v in [out.parent,*out.parents]):p.error('output must be outside Git checkout')
    out.parent.mkdir(parents=True,exist_ok=True);raw,manifest=patch(a.stock.read_bytes(),a.payload.read_bytes())
    out.write_bytes(raw);out.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
if __name__=='__main__':main()
