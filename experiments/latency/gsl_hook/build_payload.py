#!/usr/bin/env python3
"""Build a freestanding, import/relocation-free AArch64 passthrough payload."""
import argparse
import hashlib
import io
import json
import os
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

DEFAULT_NDK = Path(os.environ['SHIZUKA_NDK_BIN']) if os.environ.get('SHIZUKA_NDK_BIN') else None

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def external_output(parser, path):
    """Keep generated vendor-derived files and objects outside the checkout."""
    path=path.resolve()
    for ancestor in Path(__file__).resolve().parents:
        if (ancestor/'.git').exists() and path.is_relative_to(ancestor):
            parser.error('--output must be outside the source checkout')
    path.parent.mkdir(parents=True,exist_ok=True)
    return path

def audit_object(path):
    """Reject absolute address literals an ET_EXEC final link could conceal."""
    elf=ELFFile(io.BytesIO(path.read_bytes()))
    # AArch64 PC-relative branch/address forms and address-page low bits only.
    # Absolute pointers, TLS and GOT-import forms need a separate design.
    allowed={273,274,275,276,277,278,279,280,282,283,284,285,286,299}
    result=[]
    for section in elf.iter_sections():
        if section['sh_type'] not in ('SHT_RELA','SHT_REL'):continue
        target=elf.get_section(section['sh_info'])
        if not target['sh_flags']&2:continue
        symbols=elf.get_section(section['sh_link'])
        for relocation in section.iter_relocations():
            kind=relocation['r_info_type']
            assert kind in allowed, f'non-PIC/unsupported object relocation {kind} in {path}'
            result.append({'section':target.name,'type':kind,
                'symbol':symbols.get_symbol(relocation['r_info_sym']).name})
    return result

def inspect_payload(raw):
    elf = ELFFile(io.BytesIO(raw))
    assert elf['e_machine'] == 'EM_AARCH64' and elf.elfclass == 64 and elf.little_endian
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    undefined = [s.name for s in elf.get_section_by_name('.symtab').iter_symbols()
                 if s.name and s['st_shndx'] == 'SHN_UNDEF']
    assert not undefined, f'undefined payload imports: {undefined}'
    for section in elf.iter_sections():
        if section['sh_type'] in ('SHT_REL', 'SHT_RELA'):
            assert section.num_relocations() == 0, 'runtime relocation in payload'
        if section['sh_flags'] & 1:
            assert section['sh_size'] == 0, f'writable payload section {section.name}'
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_memsz']]
    assert len(loads) == 1 and loads[0]['p_flags'] == 5
    segment = loads[0]
    assert segment['p_filesz'] == segment['p_memsz']
    assert symbols['shizuka_gsl_gateway'] == 0x48000
    return {'rva': segment['p_vaddr'], 'data': segment.data(), 'symbols': symbols}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk-bin', type=Path, default=DEFAULT_NDK)
    parser.add_argument('--output', type=Path, required=True,
                        help='output ELF outside the source checkout; metadata/objects go beside it')
    args = parser.parse_args()
    if args.ndk_bin is None:
        parser.error('--ndk-bin or SHIZUKA_NDK_BIN is required')
    args.output=external_output(parser,args.output)
    base = Path(__file__).parent
    inputs = [base/n for n in ('gsl_passthrough.c','gsl_passthrough.h','gateway.S','payload.ld')]
    compiler=str(args.ndk_bin/'aarch64-linux-android34-clang')
    flags = ['-std=c11', '-O2',
        '-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector',
        '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-fvisibility=hidden',
        '-fPIC','-mbranch-protection=standard','-mno-outline-atomics']
    commands=[];objects=[];relocations={}
    for name in ('gsl_passthrough.c','gateway.S'):
        obj=args.output.parent/(Path(name).stem+'.o')
        command=[compiler,*flags,'-c',str(base/name),'-o',str(obj)]
        subprocess.run(command,check=True)
        commands.append(command);objects.append(obj)
        relocations[obj.name]=audit_object(obj)
    command=[compiler,'-nostdlib','-static',*[str(obj) for obj in objects],
        '-Wl,-T,'+str(base/'payload.ld'),
        '-Wl,--no-undefined','-Wl,--build-id=none','-Wl,-z,max-page-size=16384',
        '-o',str(args.output)]
    subprocess.run(command, check=True)
    commands.append(command)
    payload = inspect_payload(args.output.read_bytes())
    metadata = {'command':command,'commands':commands,'object_relocations':relocations,
        'absolute_object_address_literals':False,'inputs_sha256':{str(p):sha(p) for p in inputs},
        'output_sha256':sha(args.output),'symbols':{k:hex(v) for k,v in payload['symbols'].items()},
        'payload_bytes':len(payload['data']),'payload_rva':hex(payload['rva']),
        'undefined_imports':[],'runtime_relocations':[],'writable_payload':False,
        'provider_enabled':False,'namespace_verified':False,'device_executed':False}
    args.output.with_suffix('.build.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(json.dumps(metadata,indent=2))

if __name__ == '__main__': main()
