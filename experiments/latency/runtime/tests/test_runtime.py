#!/usr/bin/env python3
"""Exact-image offline runtime ELF/CFI checks. No Android/device access."""
import io
import os
from pathlib import Path
import struct
import sys
import unittest
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X8,UC_ARM64_REG_X30,UC_ARM64_REG_PC,UC_ARM64_REG_SP
import unicorn.arm64_const as registers
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from patch_runtime import patch

class RuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stock=Path(os.environ['SHIZUKA_GSL_STOCK']).read_bytes()
        cls.payload=Path(os.environ['SHIZUKA_RUNTIME_PAYLOAD']).read_bytes()
        cls.candidate,cls.manifest=patch(cls.stock,cls.payload)
        cls.elf=ELFFile(io.BytesIO(cls.candidate))
        p=ELFFile(io.BytesIO(cls.payload))
        cls.symbols={s.name:s['st_value'] for s in p.get_section_by_name('.symtab').iter_symbols() if s.name}

    def test_original_bytes_and_new_mappings(self):
        allow=set(range(0x20,0x30))|set(range(0x38,0x3a))|set(range(0x3c,0x3e))
        for change in self.manifest['edits']:allow.update(range(change['offset'],change['offset']+4))
        self.assertTrue(all(a==b or i in allow for i,(a,b) in enumerate(zip(self.stock,self.candidate))))
        self.assertEqual(self.elf['e_phnum'],15)
        data=self.elf.get_section_by_name('.shizuka.data')
        self.assertEqual(data['sh_flags'],3)
        self.assertGreater(data['sh_size'],1024*1024)
        self.assertFalse(any(s['p_flags']==7 for s in self.elf.iter_segments()))

    def test_cfi_extension_only_exact_type_and_target_pairs(self):
        callback=self.symbols['shizuka_runtime_callback'];worker=self.symbols['shizuka_runtime_worker']
        pairs=[(0xa99991eb1f24aa4f,callback),(0x4c550309df1cf4c1,worker)]
        cases=[(*p,True) for p in pairs]
        for kind,target in pairs:
            for delta in (-8,-4,4,8):cases.append((kind,target+delta,False))
            cases.extend([(kind^1,target,False),(kind^0x1000000000000000,target,False)])
        cases.extend([(pairs[0][0],worker,False),(pairs[1][0],callback,False),(0,0,False)])
        for base in (0x10000000,0x7100000000):
            for kind,target,allowed in cases:
                with self.subTest(base=base,kind=kind,target=target):
                    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                    for s in self.elf.iter_segments():
                        if s['p_type']!='PT_LOAD' or not s['p_memsz']:continue
                        start=s['p_vaddr']&~4095;end=(s['p_vaddr']+s['p_memsz']+4095)&~4095
                        uc.mem_map(base+start,end-start);uc.mem_write(base+s['p_vaddr'],s.data())
                    stop=base+0x800000;uc.mem_map(stop,4096)
                    uc.mem_map(0x700000,65536);uc.reg_write(UC_ARM64_REG_SP,0x708000)
                    uc.reg_write(UC_ARM64_REG_X0,kind);uc.reg_write(UC_ARM64_REG_X1,base+target)
                    uc.reg_write(UC_ARM64_REG_X2,0x12345678);uc.reg_write(UC_ARM64_REG_X30,stop)
                    seen=[]
                    def hook(u,address,size,data):
                        if address in (stop,base+0x19008):seen.append(address);u.emu_stop()
                    uc.hook_add(UC_HOOK_CODE,hook)
                    uc.emu_start(base+0x19000,stop+4,count=100)
                    self.assertEqual(seen,[stop if allowed else base+0x19008])
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),kind)
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_X1),base+target)
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_X2),0x12345678)
                    if not allowed:self.assertEqual(uc.reg_read(UC_ARM64_REG_X8),0xf1c1)

    def test_modified_stock_rejected(self):
        wrong=bytearray(self.stock);wrong[100]^=1
        with self.assertRaises(ValueError):patch(bytes(wrong),self.payload)

    def test_acdb_gateway_preserves_owner_registers_and_stack(self):
        for base in (0x10000000,0x7100000000):
            for opcode in (0xacdb0002,0xacdb0004):
                uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                for s in self.elf.iter_segments():
                    if s['p_type']!='PT_LOAD' or not s['p_memsz']:continue
                    start=s['p_vaddr']&~4095;end=(s['p_vaddr']+s['p_memsz']+4095)&~4095
                    uc.mem_map(base+start,end-start);uc.mem_write(base+s['p_vaddr'],s.data())
                stop=base+0x800000;uc.mem_map(stop,4096)
                stack=0x708000;uc.mem_map(0x700000,65536);uc.reg_write(UC_ARM64_REG_SP,stack)
                values=[opcode,0x1111,24 if opcode==0xacdb0002 else 36,0x2222,28 if opcode==0xacdb0002 else 12]
                for i,v in enumerate(values):uc.reg_write(getattr(registers,'UC_ARM64_REG_X'+str(i)),v)
                for i,v in ((17,0x3333),(20,0x7777),(21,0x4444),(26,7),(28,0x5555),(29,0x6666),(30,stop)):
                    uc.reg_write(getattr(registers,'UC_ARM64_REG_X'+str(i)),v)
                seen=[]
                def hook(u,address,size,data):
                    if address==base+self.symbols['shizuka_runtime_acdb']:
                        args=[u.reg_read(getattr(registers,'UC_ARM64_REG_X'+str(i))) for i in range(8)]
                        extra=struct.unpack('<QQ',u.mem_read(u.reg_read(UC_ARM64_REG_SP),16))
                        seen.append((args,extra));u.reg_write(UC_ARM64_REG_X0,19)
                        u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
                    elif address==stop:u.emu_stop()
                uc.hook_add(UC_HOOK_CODE,hook)
                uc.emu_start(base+self.symbols['shizuka_runtime_acdb_gateway'],stop+4,count=100)
                self.assertEqual(seen,[(values+[0x4444 if opcode==0xacdb0004 else 0x5555,stop,0x3333],(0x7777,0x6666))])
                self.assertEqual(uc.reg_read(UC_ARM64_REG_SP),stack)
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),19)
                for i,v in ((21,0x4444),(26,7),(28,0x5555),(29,0x6666),(30,stop)):
                    self.assertEqual(uc.reg_read(getattr(registers,'UC_ARM64_REG_X'+str(i))),v)

    def test_message_allocator_gateway_preserves_twelve_arguments(self):
        base=0x7100000000;uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for s in self.elf.iter_segments():
            if s['p_type']!='PT_LOAD' or not s['p_memsz']:continue
            start=s['p_vaddr']&~4095;end=(s['p_vaddr']+s['p_memsz']+4095)&~4095
            uc.mem_map(base+start,end-start);uc.mem_write(base+s['p_vaddr'],s.data())
        stop=base+0x800000;uc.mem_map(stop,4096)
        stack=0x708000;uc.mem_map(0x700000,65536);uc.reg_write(UC_ARM64_REG_SP,stack)
        first=[0x01001004,0x4009,1,16,0x1234,2,256,0];extra=[2,0x44,0x55,0x6666]
        incoming=struct.pack('<4Q',*extra);uc.mem_write(stack,incoming)
        for i,v in enumerate(first):uc.reg_write(getattr(registers,'UC_ARM64_REG_X'+str(i)),v)
        preserved=[(18,0x1818),(20,0x2020),(26,0x2626),(28,0x2828),(29,0x2929),(30,stop)]
        for i,v in [(17,0x1717),*preserved]:uc.reg_write(getattr(registers,'UC_ARM64_REG_X'+str(i)),v)
        seen=[]
        def hook(u,address,size,data):
            if address==base+self.symbols['shizuka_runtime_message_alloc']:
                args=[u.reg_read(getattr(registers,'UC_ARM64_REG_X'+str(i))) for i in range(8)]
                tail=struct.unpack('<5Q',u.mem_read(u.reg_read(UC_ARM64_REG_SP),40));seen.append((args,tail))
                u.reg_write(UC_ARM64_REG_X0,19);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
            elif address==stop:u.emu_stop()
        uc.hook_add(UC_HOOK_CODE,hook)
        uc.emu_start(base+self.symbols['shizuka_runtime_message_alloc_gateway'],stop+4,count=100)
        self.assertEqual(seen,[(first,tuple(extra+[0x1717]))])
        self.assertEqual(uc.reg_read(UC_ARM64_REG_SP),stack)
        self.assertEqual(uc.mem_read(stack,len(incoming)),incoming)
        self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),19)
        for i,v in preserved:self.assertEqual(uc.reg_read(getattr(registers,'UC_ARM64_REG_X'+str(i))),v)

    def test_metadata_strip_leaf_encoding_and_return_register_preservation(self):
        entry=self.symbols['shizuka_runtime_strip_code_pointer']
        # Unicorn2 currently treats the PAC hint as a NOP, including its MAX
        # model. Assert the actual ARM64 XPACLRI encoding separately, then model
        # only that instruction for a synthetic signed pointer. This proves
        # argument/result/real-LR handling, not hardware PAC authentication.
        expected=struct.pack('<7I',0xd503245f,0xaa1e03e8,0xaa0003fe,
                             0xd50320ff,0xaa1e03e0,0xaa0803fe,0xd65f03c0)
        for base in (0x10000000,0x7100000000):
            for modeled_pac in (False,True):
                with self.subTest(base=base,modeled_pac=modeled_pac):
                    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                    for segment in self.elf.iter_segments():
                        if segment['p_type']!='PT_LOAD' or not segment['p_memsz']:continue
                        start=segment['p_vaddr']&~4095;end=(segment['p_vaddr']+segment['p_memsz']+4095)&~4095
                        uc.mem_map(base+start,end-start);uc.mem_write(base+segment['p_vaddr'],segment.data())
                    self.assertEqual(bytes(uc.mem_read(base+entry,len(expected))),expected)
                    stop=base+0x800000;uc.mem_map(stop,4096)
                    stack=0x708000;uc.mem_map(0x700000,65536);uc.reg_write(UC_ARM64_REG_SP,stack)
                    canonical=base+0x251f0
                    argument=canonical|(0xabcd000000000000 if modeled_pac else 0)
                    preserved=[(i,0x100000+i) for i in range(1,30) if i!=8]
                    for i,value in preserved:
                        uc.reg_write(getattr(registers,'UC_ARM64_REG_X'+str(i)),value)
                    uc.reg_write(UC_ARM64_REG_X0,argument);uc.reg_write(UC_ARM64_REG_X30,stop)
                    sites=[]
                    def hook(u,address,size,data):
                        sites.append(address)
                        if address==base+entry+12:
                            self.assertEqual(u.reg_read(UC_ARM64_REG_X30),argument)
                            if modeled_pac:
                                u.reg_write(UC_ARM64_REG_X30,canonical)
                                u.reg_write(UC_ARM64_REG_PC,address+4)
                        elif address==stop:u.emu_stop()
                    uc.hook_add(UC_HOOK_CODE,hook)
                    uc.emu_start(base+entry,stop+4,count=16)
                    self.assertEqual(sites,[*[base+entry+i for i in range(0,28,4)],stop])
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),canonical)
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_X8),stop)
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_X30),stop)
                    self.assertEqual(uc.reg_read(UC_ARM64_REG_SP),stack)
                    for i,value in preserved:
                        self.assertEqual(uc.reg_read(getattr(registers,'UC_ARM64_REG_X'+str(i))),value)

if __name__=='__main__':unittest.main(verbosity=2)
