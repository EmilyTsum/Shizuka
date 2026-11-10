#!/usr/bin/env python3
"""Append a private RX passthrough payload to one exact libar-gsl ELF build.

This writes an offline candidate only. It never copies to a device, mounts,
loads a shared library, restarts a process, or issues an audio command.
"""
import argparse
import hashlib
import io
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from build_payload import inspect_payload, external_output

STOCK_SHA256 = '4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc'
PAGE = 0x4000
METADATA_RVA = 0x44000
PAYLOAD_RVA = 0x48000
PATCH_RVA = 0x38d5c
PLT_RVA = 0x38d50
PLT_BYTES = bytes.fromhex('30000090117e41f910e20b9120021fd6')
PH = struct.Struct('<IIQQQQQQ')
SH = struct.Struct('<IIQQQQIIQQ')

class Invalid(ValueError): pass

def require(value, message):
    if not value: raise Invalid(message)

def align(value, size=PAGE): return (value+size-1) & -size
def sha(raw): return hashlib.sha256(raw).hexdigest()

def branch(source, target):
    delta = target-source
    require(delta % 4 == 0 and -(1<<27) <= delta < (1<<27), 'branch out of range')
    return struct.pack('<I',0x14000000 | ((delta//4)&0x3ffffff))

def vaddr_offset(elf, address, size):
    matches = [s['p_offset']+address-s['p_vaddr'] for s in elf.iter_segments()
        if s['p_type']=='PT_LOAD' and s['p_vaddr']<=address
        and address+size <= s['p_vaddr']+s['p_filesz']]
    require(len(matches)==1,'ambiguous/non-file-backed address')
    return matches[0]

def raw_headers(raw):
    phoff=struct.unpack_from('<Q',raw,0x20)[0]
    count=struct.unpack_from('<H',raw,0x38)[0]
    require(struct.unpack_from('<H',raw,0x36)[0] == PH.size,'unexpected PH size')
    require(phoff+count*PH.size<=len(raw),'truncated PH table')
    return [list(PH.unpack_from(raw,phoff+i*PH.size)) for i in range(count)]

def validate_candidate(stock, candidate, payload):
    old=ELFFile(io.BytesIO(stock)); new=ELFFile(io.BytesIO(candidate))
    require(new['e_machine']=='EM_AARCH64' and new['e_type']=='ET_DYN','wrong output ELF type')
    require(new['e_phnum']==old['e_phnum']+2,'wrong PH count')
    require(new['e_shnum']==old['e_shnum']+2,'wrong section count')
    loads=[s for s in new.iter_segments() if s['p_type']=='PT_LOAD']
    require(all(s['p_align']==PAGE and (s['p_vaddr']-s['p_offset'])%PAGE==0 for s in loads),
            'unaligned LOAD')
    require(all(not(s['p_flags']&1 and s['p_flags']&2) for s in loads),'W+X LOAD')
    ranges=sorted((s['p_vaddr']//PAGE*PAGE,align(s['p_vaddr']+s['p_memsz'])) for s in loads)
    require(all(a[1]<=b[0] for a,b in zip(ranges,ranges[1:])),'overlapping LOAD pages')
    require(all(s['p_filesz']<=s['p_memsz'] and s['p_offset']+s['p_filesz']<=len(candidate)
                for s in loads),'invalid LOAD bounds')
    phdr=[s for s in new.iter_segments() if s['p_type']=='PT_PHDR']
    require(len(phdr)==1 and phdr[0]['p_offset']==new['e_phoff']
            and phdr[0]['p_vaddr']==METADATA_RVA
            and phdr[0]['p_filesz']==new['e_phnum']*PH.size,'invalid PT_PHDR')
    require(candidate[vaddr_offset(new,PATCH_RVA,4):vaddr_offset(new,PATCH_RVA,4)+4]
            ==branch(PATCH_RVA,PAYLOAD_RVA),'missing PLT patch')
    text=new.get_section_by_name('.shizuka.text')
    require(text and text.data()==payload['data'] and text['sh_addr']==PAYLOAD_RVA,'payload mismatch')
    for i in range(old['e_shnum']):
        a,b=old.get_section(i),new.get_section(i)
        if i==old['e_shstrndx']: continue
        require(dict(a.header)==dict(b.header),'old section metadata changed')
        require(a.data()==b.data(),'old section data changed') if a.name!='.plt' else None
    # All original bytes survive except these explicit ELF-header fields and BR.
    allowed=set(range(0x20,0x30)) | set(range(0x38,0x3a)) | set(range(0x3c,0x3e))
    patch_off=vaddr_offset(old,PATCH_RVA,4)
    allowed.update(range(patch_off,patch_off+4))
    require(all(a==b or i in allowed for i,(a,b) in enumerate(zip(stock,candidate))),
            'unexpected edit in original file bytes')
    require(candidate[vaddr_offset(new,PLT_RVA,12):vaddr_offset(new,PLT_RVA,12)+12]
            ==PLT_BYTES[:12],'ADRP/LDR/GOT resolution changed')
    return {'program_headers':new['e_phnum'],'sections':new['e_shnum'],
        'load_segments':len(loads),'load_pages_nonoverlapping':True,
        'original_bytes_preserved_except_header_and_one_branch':True,
        'original_dynamic_sections_and_relocations_preserved':True,
        'writable_executable':False}

def patch(stock, payload_raw):
    require(sha(stock)==STOCK_SHA256,'stock SHA256 mismatch; no output generated')
    old=ELFFile(io.BytesIO(stock))
    require(old.elfclass==64 and old.little_endian and old['e_machine']=='EM_AARCH64'
            and old['e_type']=='ET_DYN','unsupported input ELF')
    payload=inspect_payload(payload_raw)
    require(payload['rva']==PAYLOAD_RVA,'wrong payload RVA')
    require(len(payload['data'])<PAGE,'payload exceeds one reserved RX page')
    plt_off=vaddr_offset(old,PLT_RVA,len(PLT_BYTES))
    require(stock[plt_off:plt_off+len(PLT_BYTES)]==PLT_BYTES,'unexpected PLT instructions')
    rel=old.get_section_by_name('.rela.plt'); dynsym=old.get_section(rel['sh_link'])
    slots=[r for r in rel.iter_relocations() if dynsym.get_symbol(r['r_info_sym']).name=='acdb_ioctl']
    require(len(slots)==1 and slots[0]['r_offset']==0x3c2f8
            and slots[0]['r_info_type']==1026,'unexpected acdb GOT relocation')
    headers=raw_headers(stock)
    load_end=max(h[3]+h[6] for h in headers if h[0]==1)
    require(align(load_end)<=METADATA_RVA,'no free metadata virtual page')
    require(align(len(stock))<=METADATA_RVA,'stock file overlaps append area')
    require(sum(h[0]==6 for h in headers)==1,'missing/duplicate PT_PHDR')
    metadata_off=align(len(stock)); payload_off=metadata_off+PAGE
    count=len(headers)+2; table_size=count*PH.size
    require(table_size<=PAGE,'PH table exceeds mapping')
    for h in headers:
        if h[0]==6:
            h[:]=[6,4,metadata_off,METADATA_RVA,METADATA_RVA,table_size,table_size,8]
    headers += [[1,4,metadata_off,METADATA_RVA,METADATA_RVA,table_size,table_size,PAGE],
                [1,5,payload_off,PAYLOAD_RVA,PAYLOAD_RVA,len(payload['data']),len(payload['data']),PAGE]]
    raw=bytearray(stock)
    raw.extend(bytes(payload_off+len(payload['data'])-len(raw)))
    table=b''.join(PH.pack(*h) for h in headers)
    raw[metadata_off:metadata_off+len(table)]=table
    raw[payload_off:payload_off+len(payload['data'])]=payload['data']
    raw[plt_off+12:plt_off+16]=branch(PATCH_RVA,PAYLOAD_RVA)
    # Relocate, then extend the section table and its non-alloc string table.
    old_shoff=old['e_shoff']; old_count=old['e_shnum']
    section_headers=[list(SH.unpack_from(stock,old_shoff+i*SH.size)) for i in range(old_count)]
    shstr=bytearray(old.get_section(old['e_shstrndx']).data())
    meta_name=len(shstr); shstr.extend(b'.shizuka.phdr\0')
    text_name=len(shstr); shstr.extend(b'.shizuka.text\0')
    str_off=align(len(raw),8); raw.extend(bytes(str_off-len(raw))); raw.extend(shstr)
    shstr_header=section_headers[old['e_shstrndx']]
    shstr_header[4]=str_off; shstr_header[5]=len(shstr)
    section_headers += [[meta_name,1,2,METADATA_RVA,metadata_off,table_size,0,0,8,0],
        [text_name,1,6,PAYLOAD_RVA,payload_off,len(payload['data']),0,0,16,0]]
    shoff=align(len(raw),8); raw.extend(bytes(shoff-len(raw)))
    raw.extend(b''.join(SH.pack(*h) for h in section_headers))
    struct.pack_into('<Q',raw,0x20,metadata_off); struct.pack_into('<Q',raw,0x28,shoff)
    struct.pack_into('<H',raw,0x38,count); struct.pack_into('<H',raw,0x3c,len(section_headers))
    validation=validate_candidate(stock,bytes(raw),payload)
    manifest={'stock_sha256':sha(stock),'candidate_sha256':sha(raw),
        'payload_sha256':sha(payload_raw),'payload_bytes':len(payload['data']),
        'metadata_rva':hex(METADATA_RVA),'metadata_file_offset':hex(metadata_off),
        'payload_rva':hex(PAYLOAD_RVA),'payload_file_offset':hex(payload_off),
        'patch':{'rva':hex(PATCH_RVA),'original':PLT_BYTES[12:].hex(),
            'replacement':branch(PATCH_RVA,PAYLOAD_RVA).hex()},
        'symbols':{k:hex(v) for k,v in payload['symbols'].items() if k.startswith('shizuka_')},
        'validation':validation,'provider_enabled':False,'namespace_verified':False,
        'transformation_implemented':False,'graph_commands_sent':False,
        'device_loaded':False,'live_mount_performed':False}
    return bytes(raw),manifest

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock',type=Path,required=True,
                        help='user-supplied exact stock ELF; never bundled with this experiment')
    parser.add_argument('--payload',type=Path,required=True,help='previously built payload ELF')
    parser.add_argument('--output',type=Path,required=True,
                        help='offline candidate outside the source checkout')
    args=parser.parse_args()
    args.output=external_output(parser,args.output)
    candidate,manifest=patch(args.stock.read_bytes(),args.payload.read_bytes())
    args.output.write_bytes(candidate)
    args.output.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))

if __name__=='__main__': main()
