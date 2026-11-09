import io
import json
import os
import random
import struct
import unittest
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.common.exceptions import ELFError
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.arm64_const import *
import patch_gsl as patcher
from build_payload import inspect_payload

BASE = Path(__file__).parent

class GslHookTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        stock=os.environ.get('SHIZUKA_GSL_STOCK')
        payload_name=os.environ.get('SHIZUKA_GSL_PAYLOAD')
        payload=Path(payload_name) if payload_name else None
        if not stock or not Path(stock).is_file() or payload is None or not payload.is_file():
            raise unittest.SkipTest('requires SHIZUKA_GSL_STOCK and SHIZUKA_GSL_PAYLOAD external fixtures')
        cls.stock=Path(stock).read_bytes()
        cls.payload=payload.read_bytes()
        cls.candidate,cls.manifest=patcher.patch(cls.stock,cls.payload)
        cls.symbols=inspect_payload(cls.payload)['symbols']

    def test_exact_elf_layout_and_original_preservation(self):
        result=patcher.validate_candidate(self.stock,self.candidate,inspect_payload(self.payload))
        self.assertTrue(result['load_pages_nonoverlapping'])
        elf=ELFFile(io.BytesIO(self.candidate))
        stock=ELFFile(io.BytesIO(self.stock))
        for name in ('.dynamic','.dynsym','.dynstr','.rela.dyn','.relr.dyn','.rela.plt',
                     '.note.gnu.property','.note.gnu.build-id','.got','.got.plt'):
            self.assertEqual(elf.get_section_by_name(name).data(),stock.get_section_by_name(name).data())
        self.assertFalse(self.manifest['provider_enabled'])
        self.assertFalse(self.manifest['transformation_implemented'])

    def test_modified_stock_or_payload_is_rejected(self):
        for pos in (0,0x38d5c,len(self.stock)-1):
            raw=bytearray(self.stock);raw[pos]^=1
            with self.assertRaises(patcher.Invalid): patcher.patch(bytes(raw),self.payload)
        with self.assertRaises(patcher.Invalid):patcher.branch(0,1)
        with self.assertRaises(patcher.Invalid):patcher.branch(0,1<<27)
        with self.assertRaises(ELFError):
            inspect_payload(self.payload[:63])
        elf=ELFFile(io.BytesIO(self.payload))
        raw=bytearray(self.payload)
        # Changing the RX payload into W+X must be rejected before appending it.
        load_index=next(i for i,s in enumerate(elf.iter_segments()) if s['p_type']=='PT_LOAD')
        struct.pack_into('<I',raw,elf['e_phoff']+load_index*elf['e_phentsize']+4,7)
        with self.assertRaises(AssertionError):inspect_payload(bytes(raw))

    def machine(self,base):
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        elf=ELFFile(io.BytesIO(self.candidate))
        for segment in elf.iter_segments():
            if segment['p_type']!='PT_LOAD' or not segment['p_memsz']:continue
            start=segment['p_vaddr']//0x4000*0x4000
            end=patcher.align(segment['p_vaddr']+segment['p_memsz'])
            uc.mem_map(base+start,end-start,UC_PROT_ALL)
            uc.mem_write(base+segment['p_vaddr'],segment.data())
        return uc

    def test_actual_plt_gateway_mock_abi_randomized(self):
        rng=random.Random(0x535a)
        witnessed=[]
        for index in range(64):
            base=(0x7100000000 if index%2 else 0x1000000)
            uc=self.machine(base)
            heap=0x7300000000;mock=0x7200000000;stack=0x7400000000
            uc.mem_map(heap,0x4000);uc.mem_map(mock,0x4000);uc.mem_map(stack,0x4000)
            uc.mem_write(mock,struct.pack('<II',0xd503245f,0xd65f03c0)) # BTI c; RET
            command=bytes(rng.randrange(256) for _ in range(96))
            response=bytes(rng.randrange(256) for _ in range(96))
            uc.mem_write(heap,command);uc.mem_write(heap+0x100,response)
            opcode=rng.getrandbits(32);cmd_size=rng.randrange(97);rsp_size=rng.randrange(97)
            args=(opcode,heap,cmd_size,heap+0x100,rsp_size)
            return_rva=(0x246e4,0x247d0,0x24e20,0x38000)[index%4]
            return_pc=base+return_rva;graph=heap+0x200;sp=stack+0x3f00
            for i,value in enumerate(args):uc.reg_write(UC_ARM64_REG_X0+i,value)
            preserved={UC_ARM64_REG_X18:rng.getrandbits(64),UC_ARM64_REG_X29:rng.getrandbits(64)}
            for reg in range(UC_ARM64_REG_X19,UC_ARM64_REG_X28+1):preserved[reg]=rng.getrandbits(64)
            preserved[UC_ARM64_REG_X28]=graph
            # Unicorn exposes Q registers as integers, including callee-saved D8..D15.
            for reg in range(UC_ARM64_REG_Q8,UC_ARM64_REG_Q15+1):preserved[reg]=rng.getrandbits(128)
            for reg,value in preserved.items():uc.reg_write(reg,value)
            uc.reg_write(UC_ARM64_REG_SP,sp);uc.reg_write(UC_ARM64_REG_X30,return_pc)
            uc.mem_write(base+0x3c2f8,struct.pack('<Q',mock)) # emulate BIND_NOW GOT resolution
            calls=[];gateway=[];result=rng.getrandbits(32)
            def instruction(machine,address,size,opaque):
                if address==base+self.symbols['shizuka_gsl_acdb_wrapper']:
                    gateway.append(tuple(machine.reg_read(UC_ARM64_REG_X0+i) for i in range(8)))
                if address==mock:
                    calls.append(tuple(machine.reg_read(UC_ARM64_REG_X0+i) for i in range(5)))
                    self.assertEqual(machine.reg_read(UC_ARM64_REG_SP),sp)
                    # An original callee may write its response. The wrapper must
                    # preserve precisely that result and make no additional edit.
                    if rsp_size:machine.mem_write(heap+0x100,b'\x7b')
                    machine.reg_write(UC_ARM64_REG_X0,result)
            uc.hook_add(UC_HOOK_CODE,instruction)
            uc.emu_start(base+patcher.PLT_RVA,return_pc,count=100)
            self.assertEqual(calls,[args]);self.assertEqual(gateway,[args+(graph,return_pc,mock)])
            self.assertEqual(uc.reg_read(UC_ARM64_REG_PC),return_pc)
            self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),result)
            self.assertEqual(uc.reg_read(UC_ARM64_REG_SP),sp)
            for reg,value in preserved.items():self.assertEqual(uc.reg_read(reg),value)
            self.assertEqual(bytes(uc.mem_read(heap,len(command))),command)
            expected=(b'\x7b'+response[1:]) if rsp_size else response
            self.assertEqual(bytes(uc.mem_read(heap+0x100,len(response))),expected)
            witnessed.append(return_rva)
        self.assertEqual(set(witnessed),{0x246e4,0x247d0,0x24e20,0x38000})

    def test_payload_bti_tail_branch_and_pure_policy(self):
        payload=inspect_payload(self.payload)
        # BR x16 retains call-compatible BTYPE, unlike a generic BR x7 on BTI.
        offset=self.symbols['shizuka_gsl_acdb_wrapper']-payload['rva']
        self.assertEqual(payload['data'][offset:offset+12],
                         struct.pack('<III',0xd503245f,0xaa0703f0,0xd61f0200))
        for argument,expected in ((0x246e4,1),(0x247d0,2),(0x24e20,3),(0,0),(0x246e0,0),(1<<40,0)):
            uc=self.machine(0x1000000);uc.reg_write(UC_ARM64_REG_X0,argument)
            uc.reg_write(UC_ARM64_REG_X30,0x1000000+0x38000)
            uc.emu_start(0x1000000+self.symbols['shizuka_gsl_hook_classify'],0x1000000+0x38000,count=100)
            self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),expected)
        uc=self.machine(0x1000000);uc.reg_write(UC_ARM64_REG_X30,0x1000000+0x38000)
        uc.emu_start(0x1000000+self.symbols['shizuka_gsl_hook_policy'],0x1000000+0x38000,count=100)
        self.assertEqual(uc.reg_read(UC_ARM64_REG_X0),0x10000)

if __name__=='__main__':unittest.main(verbosity=2)
