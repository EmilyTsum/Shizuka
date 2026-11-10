"""Synthetic-only native graph/transaction tests; no vendor file/device needed."""
import ctypes as C
import os
from pathlib import Path
import platform
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

BASE=Path(__file__).parent
SG,CONT,RD,WR,CLIENT=0xb00000ab,0xe00000e6,0x43f7,0x43fa,0x535a6101
def words(*v):return struct.pack('<'+'I'*len(v),*v)
def param(mid,pid,data):return words(mid,pid,len(data),0)+data+bytes([0xa5])*((-len(data))%8)
def props(items):return words(len(items))+b''.join(words(pid,len(data))+data for pid,data in items)
def sg(sgid):return param(1,0x08001001,words(1,sgid,2)+words(0x0800100f,4,2,0x08001010,4,1))

def fixture(expanded=False,client=CLIENT,island=False):
    # Entirely synthetic calibration payload, with nonzero padding and opaque
    # parameters to make accidental loss/reordering observable.
    prefix=sg(0xb000006c)+param(1,0x08001061,words(1,0x4011,0xc0000001,0x4022,0xc0000002,0))
    prefix+=param(1,0x08001004,words(1,WR,1,0x4098,2)) # Existing external encoded edge stays intact.
    containers=words(2)
    for cid in (0xe00000e5,CONT):
        entries=[(0x08001014,words(2)),(0x080010cb,words(client if expanded and (cid==CONT or island) else 0xffffffff)),
                 (0x08001999,b'opaque00')]
        if island:
            entries=[(0x08001014,words(2)),(0x08001013,words(4096)),
                     (0x08001011,words(1,0x0b001001)),(0x08001012,words(0xffffffff)),
                     (0x080010cb,words(client if expanded else 0xffffffff)),
                     (0x08001174,words(1)),(0x0800124d,words(1))]
        containers+=words(cid,len(entries))+b''.join(words(p,len(d))+d for p,d in entries)
    group1=[(0x07001010,0x43f3),(0x0700101a,0x43f4)]
    group2=[(0x07001001 if expanded else 0x0700101a,RD),(0x07001003,0x43f8),
            (0x07001000 if expanded else 0x0700101a,WR),(0x07001082,0x43fb)]
    if island:
        group1 += [(0x07001011,0x43f5),(0x07001041,0x43f6)]
        if expanded:
            group1 += [group2[0],group2[1],group2[3]]
            group2 = [group2[2]]
    modules=words(2)+b''.join(words(SG,cid,len(items))+b''.join(words(*v) for v in items)
        for cid,items in ((0xe00000e5,group1),(CONT,group2)))
    port_items=[(iid,0 if expanded and iid==WR else (5 if island and iid==0x43f3 else 1),
                0 if expanded and iid==RD else (3 if island and iid==0x43f5 else 1))
                for iid in ((0x43f3,0x43f4,0x43f5,0x43f6,RD,0x43f8,WR,0x43fb) if island
                            else (0x43f3,0x43f4,RD,0x43f8,WR,0x43fb))]
    ports=words(len(port_items))+b''.join(words(iid,1,0x08001015,8,ni,no) for iid,ni,no in port_items)
    edges=[(0x43f3,1,0x43f4,2),(0x43f8,1,0x43f7,2),(0x43f7,1,0x43fb,2),
           (0x43fb,1,0x43fa,2)]
    if expanded:edges=[(0x43f3,1,0x43f4,2),(0x43f8,1,0x43fb,2),(0x43fb,1,RD,2)]
    if island:
        edges += [(0x43f4,1,0x43f5,2),(0x43f5,1,0x43f6,2),
                  (0x43f6,1,0x43f8,2),(0x43f5,3,0x44e9,2)]
    connections=words(len(edges))+b''.join(words(*edge) for edge in edges)
    controls=words(1,0x4edd,0xc0000001,0x43fb,0xc0000001,2,
                   0x08001062,8,1,0x080010d7,0x08001998,4,0xdeadbeef)
    target=sg(SG)+b''.join(param(1,p,d) for p,d in ((0x08001000,containers),(0x08001002,modules),
        (0x08001003,ports),(0x08001004,connections),(0x08001061,controls)))
    target+=param(0x43fb,0x08001997,bytes(range(59)))
    if expanded:target+=param(RD,0x0800114d,words(1,client))+param(WR,0x080010c0,words(1,client))+param(RD,0x08001007,words(1,1))
    return prefix+target+sg(0xb0000099)+param(0x4099,0x08001996,b'unchanged tail')

def parameters(blob):
    pos=0;out=[]
    while pos<len(blob):
        mid,pid,n,err=struct.unpack_from('<4I',blob,pos)
        end=pos+16+((n+7)&~7);out.append((pos,mid,pid,blob[pos+16:pos+16+n],blob[pos:end]));pos=end
    return out

class Key(C.Structure):_fields_=[('key',C.c_uint32),('value',C.c_uint32)]
class Scope(C.Structure):_fields_=[('graph',C.c_size_t),('thread',C.c_size_t),('epoch',C.c_uint64),
    ('source_port',C.c_uint32),('client_port',C.c_uint32),('flags',C.c_uint32),
    ('open_refs',C.c_uint32),('start_refs',C.c_uint32),('stop_refs',C.c_uint32)]
class Provider(C.Structure):_fields_=[('revision',C.c_uint64),('enabled',C.c_uint32),('hq_extra_ttp_us',C.c_uint32)]
class Call(C.Structure):_fields_=[('caller_rva',C.c_uint32),('opcode',C.c_uint32),('command_size',C.c_uint32),
    ('response_size',C.c_uint32),('original_status',C.c_int32),('original_spf_size',C.c_uint32),
    ('sgids',C.POINTER(C.c_uint32)),('num_sgids',C.c_uint32),('keys',C.POINTER(Key)),('num_keys',C.c_uint32),
    ('spf',C.POINTER(C.c_uint8)),('spf_capacity',C.c_size_t)]
class Transaction(C.Structure):_fields_=[('scope',Scope),('provider',Provider),('phase',C.c_uint32),
    ('original_size',C.c_uint32),('num_sgids',C.c_uint32),('num_keys',C.c_uint32),('profile_confirmed',C.c_uint32),
    ('sgids',C.c_uint32*128),('keys',Key*64)]
class Result(C.Structure):_fields_=[('advertised_size',C.c_uint32),('frozen_revision',C.c_uint64)]
class CalCall(C.Structure):_fields_=[('caller_rva',C.c_uint32),('opcode',C.c_uint32),('command_size',C.c_uint32),
    ('response_size',C.c_uint32),('original_status',C.c_int32),('sgids',C.POINTER(C.c_uint32)),('num_sgids',C.c_uint32),
    ('current_gkv',C.POINTER(Key)),('num_gkv',C.c_uint32),('prior_ckv',C.POINTER(Key)),('new_ckv',C.POINTER(Key)),
    ('num_prior_ckv',C.c_uint32),('num_new_ckv',C.c_uint32),('data',C.POINTER(C.c_uint8)),
    ('data_size',C.c_uint32),('capacity',C.c_size_t)]
class CalTransaction(C.Structure):_fields_=[('scope',Scope),('pending',C.c_uint32),('capacity',C.c_uint32),
    ('num_sgids',C.c_uint32),('num_prior_ckv',C.c_uint32),('num_new_ckv',C.c_uint32),('sgids',C.c_uint32*128),
    ('prior_ckv',Key*64),('new_ckv',Key*64)]
class CalResult(C.Structure):_fields_=[('actual_size',C.c_uint32),('removed',C.c_uint32)]

class RelayGraphTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which(os.environ.get('CC','clang'))
        if not compiler:raise unittest.SkipTest('host C compiler required')
        cls.temp=tempfile.TemporaryDirectory(prefix='shizuka-relay-graph-')
        cls.library=Path(cls.temp.name)/'relay-host.so'
        subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding',
            '-fno-builtin','-fno-stack-protector','-fPIC','-dynamiclib' if platform.system()=='Darwin' else '-shared',
            str(BASE/'relay_graph.c'),str(BASE/'relay_transaction.c'),str(BASE/'relay_calibration.c'),'-o',str(cls.library)],check=True)
        cls.lib=C.CDLL(str(cls.library));p=C.POINTER(C.c_uint8)
        cls.lib.shizuka_relay_graph_measure.argtypes=[p,C.c_size_t,C.c_uint32,C.POINTER(C.c_size_t)]
        cls.lib.shizuka_relay_graph_transform.argtypes=[p,C.c_size_t,C.c_uint32,p,C.c_size_t,C.POINTER(C.c_size_t)]
        cls.lib.shizuka_relay_graph_verify.argtypes=[p,C.c_size_t,C.c_uint32]
        cls.lib.shizuka_relay_after_acdb.argtypes=[C.POINTER(Transaction),C.POINTER(Scope),C.POINTER(Provider),
            C.POINTER(Call),p,C.c_size_t,C.POINTER(Result)]
        cls.lib.shizuka_relay_open_complete.argtypes=[C.POINTER(Transaction),C.POINTER(Scope),C.c_int]
        cls.lib.shizuka_relay_confirm_profile.argtypes=[C.POINTER(Transaction),C.POINTER(Scope)]+[C.c_uint32]*6
        cls.lib.shizuka_relay_active_delta.argtypes=[C.POINTER(Transaction)];cls.lib.shizuka_relay_active_delta.restype=C.c_uint32
        cls.lib.shizuka_relay_close.argtypes=[C.POINTER(Transaction),C.POINTER(Scope)]
        cls.lib.shizuka_relay_after_calibration.argtypes=[C.POINTER(Transaction),C.POINTER(CalTransaction),C.POINTER(Scope),
            C.POINTER(CalCall),C.POINTER(CalResult)]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def transform(self,blob,capacity=None):
        source=(C.c_uint8*len(blob)).from_buffer_copy(blob)
        capacity=capacity if capacity is not None else len(blob)+56
        output=(C.c_uint8*capacity)(*([0x7b]*capacity));written=C.c_size_t(123)
        result=self.lib.shizuka_relay_graph_transform(source,len(blob),CLIENT,output,capacity,C.byref(written))
        self.assertEqual(bytes(source),blob)
        return result,bytes(output[:written.value]),bytes(output),written.value
    def test_exact_synthetic_oracle_and_imcl_opaque_preservation(self):
        original=fixture();result,out,_,n=self.transform(original)
        self.assertEqual((result,n),(0,len(original)+56));self.assertEqual(out,fixture(True))
        for _,mid,pid,data,raw in parameters(original):
            if pid==0x08001061 or mid!=1:self.assertIn(raw,out)
        self.assertEqual(self.lib.shizuka_relay_graph_verify((C.c_uint8*len(out)).from_buffer_copy(out),len(out),CLIENT),0)
    def test_no_partial_output_on_bounds_or_ownership_shape_failure(self):
        for blob in (fixture(True),fixture().replace(words(0x080010cb,4,0xffffffff),words(0x080010cb,4,CLIENT)),
                     fixture().replace(words(0x07001082,0x43fb),words(0x07001081,0x43fb))):
            result,_,out,n=self.transform(blob);self.assertLess(result,0);self.assertEqual(n,0);self.assertEqual(out,bytes([0x7b])*len(out))
        result,_,out,n=self.transform(fixture(),len(fixture())+55)
        self.assertEqual((result,n),(-2,0));self.assertEqual(out,bytes([0x7b])*len(out))
        source=(C.c_uint8*(len(fixture())+56)).from_buffer_copy(fixture()+bytes(56));n=C.c_size_t()
        self.assertEqual(self.lib.shizuka_relay_graph_transform(source,len(fixture()),CLIENT,source,len(source),C.byref(n)),-4)
    def test_truncation_corruption_and_deterministic_mutations(self):
        original=fixture();boundaries={0,len(original)}|{p[0] for p in parameters(original)}
        for n in range(len(original)):
            if n in boundaries:continue
            result,_,_,written=self.transform(original[:n]);self.assertLess(result,0);self.assertEqual(written,0)
        rng=random.Random(0x535a)
        for _ in range(1200):
            raw=bytearray(original);index=rng.randrange(len(raw));raw[index]^=1<<rng.randrange(8)
            result,out,_,n=self.transform(bytes(raw))
            if result==0:
                self.assertEqual(n,len(raw)+56)
                self.assertEqual(self.lib.shizuka_relay_graph_verify((C.c_uint8*len(out)).from_buffer_copy(out),len(out),CLIENT),0)
            else:self.assertEqual(n,0)
    def context(self):
        blob=fixture();scope=Scope(0x10000,0x20000,7,0x2012,CLIENT,15,1,0,0);provider=Provider(10,1,150000)
        ids=(C.c_uint32*3)(0xb000006c,SG,0xb0000099)
        keys=(Key*4)(Key(0xa2000000,0xa2000003),Key(0xb4000000,0xb4000002),Key(0xb5000000,0xb5000003),Key(0xa1000000,0xa1000011))
        source=(C.c_uint8*(len(blob)+56)).from_buffer_copy(blob+bytes(56));workspace=(C.c_uint8*len(source))()
        call=Call(0x246e4,0xacdb0002,24,28,0,len(blob),ids,len(ids),keys,len(keys),None,0)
        return blob,scope,provider,source,workspace,call,ids,keys
    def after(self,t,s,p,c,w):
        out=Result();status=self.lib.shizuka_relay_after_acdb(C.byref(t),C.byref(s),C.byref(p),C.byref(c),w,len(w),C.byref(out))
        return status,out
    def active(self):
        values=self.context();blob,s,p,source,w,c,ids,keys=values;t=Transaction()
        self.assertEqual(self.after(t,s,p,c,w)[0],1);c.caller_rva=0x247d0
        self.assertEqual(self.after(t,s,p,c,w)[0],1);c.caller_rva=0x24e20
        c.spf=C.cast(source,C.POINTER(C.c_uint8));c.spf_capacity=len(source)
        self.assertEqual(self.after(t,s,p,c,w)[0],2)
        self.assertEqual(self.lib.shizuka_relay_open_complete(C.byref(t),C.byref(s),1),1)
        return t,values
    def test_provider_revision_frozen_size_fill_and_exact_ack_epoch(self):
        blob,s,p,source,w,c,ids,keys=self.context();t=Transaction()
        status,out=self.after(t,s,p,c,w);self.assertEqual((status,out.advertised_size,out.frozen_revision),(1,len(blob)+56,10))
        p.revision=11;p.hq_extra_ttp_us=0;p.enabled=0;c.caller_rva=0x247d0
        self.assertEqual(self.after(t,s,p,c,w)[0],1)
        c.caller_rva=0x24e20;c.spf=C.cast(source,C.POINTER(C.c_uint8));c.spf_capacity=len(source)
        status,out=self.after(t,s,p,c,w);self.assertEqual((status,out.frozen_revision),(2,10));self.assertEqual(bytes(source),fixture(True))
        self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),0)
        s.epoch+=1;self.assertEqual(self.lib.shizuka_relay_open_complete(C.byref(t),C.byref(s),1),0);s.epoch-=1
        s.thread+=1 # APM callback is allowed on another thread in the same owner epoch.
        self.assertEqual(self.lib.shizuka_relay_open_complete(C.byref(t),C.byref(s),1),1)
        args=(C.byref(t),C.byref(s),1,0x07001082,4096,44100,3,0)
        self.assertEqual(self.lib.shizuka_relay_confirm_profile(*args),1)
        self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),150000)
        self.lib.shizuka_relay_close(C.byref(t),C.byref(s));self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),0)
    def test_initial_gates_bypass_and_late_failure_aborts_without_input_edit(self):
        for flag in range(15):
            blob,s,p,source,w,c,ids,keys=self.context();s.flags=flag;t=Transaction()
            self.assertEqual(self.after(t,s,p,c,w)[0],0);self.assertEqual(t.phase,0);self.assertEqual(bytes(source[:len(blob)]),blob)
        for field,value in (('open_refs',2),('start_refs',1),('stop_refs',1)):
            blob,s,p,source,w,c,ids,keys=self.context();setattr(s,field,value);self.assertEqual(self.after(Transaction(),s,p,c,w)[0],0)
        for change in ('size','keys','thread','namespace','registration','source','topology','capacity','stage'):
            blob,s,p,source,w,c,ids,keys=self.context();t=Transaction();self.assertEqual(self.after(t,s,p,c,w)[0],1)
            c.caller_rva=0x247d0;self.assertEqual(self.after(t,s,p,c,w)[0],1)
            c.caller_rva=0x24e20;c.spf=C.cast(source,C.POINTER(C.c_uint8));c.spf_capacity=len(source)
            if change=='size':c.original_spf_size-=8
            if change=='keys':keys[2].value=0xb5000001
            if change=='thread':s.thread+=1
            if change=='namespace':s.flags&=~2
            if change=='registration':s.flags&=~8
            if change=='source':s.source_port=0x2013
            if change=='topology':source[parameters(blob)[3][0]+16]=0xff
            if change=='capacity':c.spf_capacity-=1
            if change=='stage':c.caller_rva=0x246e4
            before=bytes(source);self.assertEqual(self.after(t,s,p,c,w)[0],-1);self.assertEqual(t.phase,5);self.assertEqual(bytes(source),before)
    def test_registered_stock_source_port_required_before_and_after_size_growth(self):
        # DZIF GSL uses its ordinary 0x2010..0x2100 pool. Numeric height is not
        # registration/ownership evidence; the embedding owner supplies bit8.
        blob,s,p,source,w,c,ids,keys=self.context();t=Transaction()
        self.assertEqual((s.source_port,s.flags),(0x2012,15))
        status,out=self.after(t,s,p,c,w)
        self.assertEqual((status,out.advertised_size),(1,len(blob)+56))
        self.assertEqual(t.scope.source_port,0x2012)
        for port,flags in ((0x2012,7),(0,15),(0xffffffff,15),(CLIENT,15)):
            with self.subTest(source_port=port,flags=flags):
                blob,s,p,source,w,c,ids,keys=self.context();s.source_port=port;s.flags=flags
                t=Transaction();before=bytes(source)
                self.assertEqual(self.after(t,s,p,c,w)[0],0)
                self.assertEqual(t.phase,0);self.assertEqual(bytes(source),before)
        blob,s,p,source,w,c,ids,keys=self.context();t=Transaction()
        self.assertEqual(self.after(t,s,p,c,w)[0],1)
        s.flags=7;c.caller_rva=0x247d0;before=bytes(source)
        self.assertEqual(self.after(t,s,p,c,w)[0],-1)
        self.assertEqual((t.phase,t.profile_confirmed),(5,0));self.assertEqual(bytes(source),before)
    def test_other_codec_voice_and_unconfirmed_profile_never_gain_delta(self):
        for code in (0xb5000001,0xb5000005,0xb5000007,0xb5000008):
            blob,s,p,source,w,c,ids,keys=self.context();keys[2].value=code
            self.assertEqual(self.after(Transaction(),s,p,c,w)[0],0)
        blob,s,p,source,w,c,ids,keys=self.context();keys[3].key=0xa3000000
        self.assertEqual(self.after(Transaction(),s,p,c,w)[0],0)
        t,values=self.active();blob,s,p,source,w,c,ids,keys=values
        for args in ((0,0x07001082,4096,44100,3,0),(1,0x07001008,4096,44100,3,0),
                     (1,0x07001082,8192,44100,3,0),(1,0x07001082,4096,48000,3,0),
                     (1,0x07001082,4096,44100,2,0),(1,0x07001082,4096,44100,3,1)):
            self.assertEqual(self.lib.shizuka_relay_confirm_profile(C.byref(t),C.byref(s),*args),0)
            self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),0)
    def cal_after(self,t,ct,s,call):
        out=CalResult();rc=self.lib.shizuka_relay_after_calibration(C.byref(t),C.byref(ct),C.byref(s),C.byref(call),C.byref(out))
        return rc,out
    def test_other_sg_default_and_different_gkv_calibration_bypass_unchanged(self):
        other_keys=(Key*1)(Key(0xa3000000,0xdeadbeef))
        other_ckv=(Key*1)(Key(0xb5000000,0xb5000001))
        data=param(RD,0x08001026,words(1))+param(0x4011,0x08001999,b'other SG calibration')
        for default,duplicate in ((True,False),(False,False),(True,True),(False,True)):
            with self.subTest(default_calibration=default,duplicate_unrelated_id=duplicate):
                # ACDB owns validation of unrelated SG vectors. Bounded
                # absence of our target is enough to avoid touching the call.
                other_ids=(C.c_uint32*2)(0xb000006c,0xb000006c if duplicate else 0xb0000099)
                t,values=self.active();blob,s,p,source,w,c,ids,keys=values;ct=CalTransaction()
                owner_before,pending_before=bytes(t),bytes(ct)
                call=CalCall(0x1e1c0,0xacdb0004,36,12,0,other_ids,len(other_ids),
                    keys if default else other_keys,len(keys) if default else len(other_keys),
                    None if default else other_ckv,None if default else other_ckv,
                    0 if default else 1,0 if default else 1,None,len(data),0)
                for fill,original_status in ((False,0),(True,0),(False,7),(True,7)):
                    buffer=(C.c_uint8*len(data)).from_buffer_copy(data)
                    call.caller_rva=0x1e2d8 if fill else 0x1e1c0
                    call.original_status=original_status
                    call.data=buffer if fill else None;call.capacity=len(data) if fill else 0
                    call_before=bytes(call);rc,out=self.cal_after(t,ct,s,call)
                    self.assertEqual((rc,out.actual_size,out.removed),(0,len(data),0))
                    self.assertEqual(bytes(call),call_before);self.assertEqual(bytes(buffer),data)
                    self.assertEqual(bytes(t),owner_before);self.assertEqual(bytes(ct),pending_before)
                    self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),0)
    def test_pending_owned_calibration_survives_other_sg_query_and_fill(self):
        other_ids=(C.c_uint32*1)(0xb000006c)
        other_keys=(Key*1)(Key(0xb4000000,0xb4000001))
        other_ckv=(Key*1)(Key(0xa1000000,0xa1000022))
        remove=param(RD,0x08001026,words(1));keep=param(0x43fb,0x08001999,b'owned encoder')
        data=remove+keep
        for confirmed in (False,True):
            with self.subTest(profile_confirmed=confirmed):
                t,values=self.active();blob,s,p,source,w,c,ids,keys=values;ct=CalTransaction()
                if confirmed:
                    self.assertEqual(self.lib.shizuka_relay_confirm_profile(
                        C.byref(t),C.byref(s),1,0x07001082,4096,44100,3,0),1)
                delta=150000 if confirmed else 0
                owned=CalCall(0x1e1c0,0xacdb0004,36,12,0,ids,len(ids),keys,len(keys),None,None,0,0,None,len(data),0)
                self.assertEqual(self.cal_after(t,ct,s,owned)[0],0);self.assertEqual(ct.pending,1)
                owner_before,pending_before=bytes(t),bytes(ct)
                for default in (True,False):
                    unrelated=CalCall(0x1e1c0,0xacdb0004,36,12,0,other_ids,len(other_ids),
                        keys if default else other_keys,len(keys) if default else len(other_keys),
                        None if default else other_ckv,None if default else other_ckv,
                        0 if default else 1,0 if default else 1,None,len(data),0)
                    for fill,original_status in ((False,0),(True,0),(False,7),(True,7)):
                        buffer=(C.c_uint8*len(data)).from_buffer_copy(data)
                        unrelated.caller_rva=0x1e2d8 if fill else 0x1e1c0
                        unrelated.original_status=original_status
                        unrelated.data=buffer if fill else None;unrelated.capacity=len(data) if fill else 0
                        rc,out=self.cal_after(t,ct,s,unrelated)
                        self.assertEqual((rc,out.actual_size,out.removed),(0,len(data),0))
                        self.assertEqual(bytes(buffer),data);self.assertEqual(bytes(t),owner_before)
                        self.assertEqual(bytes(ct),pending_before)
                        self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),delta)
                buffer=(C.c_uint8*len(data)).from_buffer_copy(data)
                owned.caller_rva=0x1e2d8;owned.data=buffer;owned.capacity=len(data)
                rc,out=self.cal_after(t,ct,s,owned)
                self.assertEqual((rc,out.actual_size,out.removed),(2,len(keep),1))
                self.assertEqual(bytes(buffer[:out.actual_size]),keep);self.assertEqual(ct.pending,0)
                self.assertEqual(bytes(t),owner_before)
                self.assertEqual(self.lib.shizuka_relay_active_delta(C.byref(t)),delta)
    def test_owned_logger_calibration_compacts_only_reviewed_parameters(self):
        t,values=self.active();blob,s,p,source,w,c,ids,keys=values;ct=CalTransaction()
        remove=[param(RD,0x08001026,words(1)),param(WR,0x08001031,bytes(range(13))),param(RD,0x08001313,words(0))]
        keep=[param(0x4011,0x08001031,b'other logger'),param(RD,0x0800114d,words(1,CLIENT)),param(0x43fb,0x08001183,bytes(range(59)))]
        data=remove[0]+keep[0]+remove[1]+keep[1]+remove[2]+keep[2]
        call=CalCall(0x1e1c0,0xacdb0004,36,12,0,ids,len(ids),keys,len(keys),None,None,0,0,None,len(data),0)
        status,out=self.cal_after(t,ct,s,call);self.assertEqual((status,out.actual_size),(0,len(data)))
        buffer=(C.c_uint8*len(data)).from_buffer_copy(data);call.caller_rva=0x1e2d8;call.data=buffer;call.capacity=len(data)
        status,out=self.cal_after(t,ct,s,call)
        self.assertEqual((status,out.removed,out.actual_size),(2,3,len(b''.join(keep))))
        self.assertEqual(bytes(buffer[:out.actual_size]),b''.join(keep));self.assertEqual(ct.pending,0)
    def test_calibration_bad_scope_malformed_and_empty_handling(self):
        for change in ('thread','ckv','truncated','epoch','gkv','unowned'):
            t,values=self.active();blob,s,p,source,w,c,ids,keys=values;ct=CalTransaction()
            data=param(RD,0x08001031,words(1))+param(0x43fb,0x08001183,bytes(range(59)))
            call=CalCall(0x1e1c0,0xacdb0004,36,12,0,ids,len(ids),keys,len(keys),None,None,0,0,None,len(data),0)
            self.assertEqual(self.cal_after(t,ct,s,call)[0],0)
            buffer=(C.c_uint8*len(data)).from_buffer_copy(data);call.caller_rva=0x1e2d8;call.data=buffer;call.capacity=len(data)
            if change=='thread':s.thread+=1
            if change=='ckv':call.num_new_ckv=1
            if change=='truncated':call.data_size-=1
            if change=='epoch':s.epoch+=1
            if change=='gkv':keys[2].value=0xb5000001
            if change=='unowned':t.phase=6
            before=bytes(buffer);rc,_=self.cal_after(t,ct,s,call)
            self.assertEqual(rc,0 if change in ('epoch','unowned') else -1);self.assertEqual(bytes(buffer),before)
        t,values=self.active();blob,s,p,source,w,c,ids,keys=values;ct=CalTransaction()
        data=param(RD,0x08001026,words(1));call=CalCall(0x1e1c0,0xacdb0004,36,12,0,ids,len(ids),keys,len(keys),None,None,0,0,None,len(data),0)
        self.assertEqual(self.cal_after(t,ct,s,call)[0],0)
        buffer=(C.c_uint8*len(data)).from_buffer_copy(data);call.caller_rva=0x1e2d8;call.data=buffer;call.capacity=len(data)
        rc,out=self.cal_after(t,ct,s,call);self.assertEqual((rc,out.actual_size,out.removed),(3,0,1))

class RawIslandGraphTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which(os.environ.get('CC','clang'))
        if not compiler:raise unittest.SkipTest('host C compiler required')
        cls.temp=tempfile.TemporaryDirectory(prefix='shizuka-raw-island-')
        cls.lib={}
        for enabled in (0,1):
            library=Path(cls.temp.name)/f'graph-{enabled}.so'
            subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding',
                '-fno-builtin','-fno-stack-protector','-fPIC',f'-DSHIZUKA_RELAY_RAW_ISLAND={enabled}',
                '-dynamiclib' if platform.system()=='Darwin' else '-shared',str(BASE/'relay_graph.c'),
                '-o',str(library)],check=True)
            lib=C.CDLL(str(library));p=C.POINTER(C.c_uint8)
            lib.shizuka_relay_graph_measure.argtypes=[p,C.c_size_t,C.c_uint32,C.POINTER(C.c_size_t)]
            lib.shizuka_relay_graph_transform.argtypes=[p,C.c_size_t,C.c_uint32,p,C.c_size_t,C.POINTER(C.c_size_t)]
            lib.shizuka_relay_graph_verify.argtypes=[p,C.c_size_t,C.c_uint32]
            cls.lib[enabled]=lib
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def transform(self,blob,enabled=1,capacity=None):
        source=(C.c_uint8*len(blob)).from_buffer_copy(blob)
        capacity=capacity if capacity is not None else len(blob)+56
        output=(C.c_uint8*capacity)(*([0x7b]*capacity));written=C.c_size_t(123)
        rc=self.lib[enabled].shizuka_relay_graph_transform(source,len(blob),CLIENT,output,capacity,C.byref(written))
        self.assertEqual(bytes(source),blob)
        return rc,bytes(output[:written.value]),bytes(output),written.value
    def test_default_build_and_explicit_zero_retain_original_topology(self):
        for blob,expected in ((fixture(),fixture(True)),(fixture(island=True),None)):
            rc,out,_,size=self.transform(blob,0)
            self.assertEqual((rc,size),(0,len(blob)+56))
            if expected is not None:self.assertEqual(out,expected)
            for _,_,pid,data,_ in parameters(out):
                if pid!=0x08001002:continue
                self.assertIn(words(SG,CONT,4),data)
                self.assertIn(words(0x07001082,0x43fb),data)
    def test_exact_island_oracle_and_unrelated_bytes_preserved(self):
        original=fixture(island=True);rc,out,_,size=self.transform(original)
        self.assertEqual((rc,size),(0,len(original)+56))
        self.assertEqual(out,fixture(True,island=True))
        self.assertEqual(self.lib[1].shizuka_relay_graph_verify((C.c_uint8*len(out)).from_buffer_copy(out),len(out),CLIENT),0)
        for _,mid,pid,_,raw in parameters(original):
            if pid==0x08001061 or mid!=1:self.assertIn(raw,out)
        self.assertEqual(self.lib[0].shizuka_relay_graph_verify((C.c_uint8*len(out)).from_buffer_copy(out),len(out),CLIENT),-1)
    def test_incomplete_or_foreign_e5_contract_rejected_without_output(self):
        original=fixture(island=True)
        bad=[fixture(),fixture(True,island=True),
             original.replace(words(0x07001041,0x43f6),words(0x07001040,0x43f6)),
             original.replace(words(SG,0xe00000e5,4),words(0xb0000099,0xe00000e5,4)),
             original.replace(words(0x43f6,1,0x43f8,2),words(0x43f6,3,0x43f8,2)),
             original.replace(words(0x08001013,4,4096),words(0x08001013,4,2048)),
             original.replace(words(0x08001011,8,1,0x0b001001),words(0x08001011,8,1,0x0b001002)),
             original.replace(words(0x43f5,1,0x08001015,8,1,3),words(0x43f5,1,0x08001015,8,1,2))]
        for blob in bad:
            with self.subTest(blob=blob[:40]):
                rc,_,output,size=self.transform(blob)
                self.assertLess(rc,0);self.assertEqual(size,0);self.assertEqual(output,bytes([0x7b])*len(output))
        rc,_,output,size=self.transform(original,capacity=len(original)+55)
        self.assertLess(rc,0);self.assertEqual(size,0);self.assertEqual(output,bytes([0x7b])*len(output))

if __name__=='__main__':unittest.main(verbosity=2)
