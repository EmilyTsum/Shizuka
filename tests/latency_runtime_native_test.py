# SPDX-License-Identifier: GPL-2.0-only
"""Exercise wire parsers, owned graph transactions and asynchronous transport."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
R=ROOT/'experiments/latency/runtime'
G=ROOT/'experiments/latency/gsl_hook'
T=ROOT/'experiments/latency/transport/native'
CC=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')

class NativeLatency(unittest.TestCase):
    def run_case(self,name,sources,test,extra=()):
        self.assertTrue(CC,'A host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='shizuka-relay-test-') as folder:
            output=Path(folder)/name
            flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',*extra]
            includes=['-I'+str(p) for p in (R,R/'include',G,T/'include')]
            result=subprocess.run([CC,*flags,*includes,*map(str,sources),str(test),'-o',str(output)],text=True,capture_output=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(output)],text=True,capture_output=True,timeout=45)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_encoded_buffer_and_read_only_attestation(self):
        self.run_case('transform',[R/'src/encoded_transform.c'],R/'tests/encoded_transform_test.c')
    def test_vendor_allocator_and_receive_ownership(self):
        self.run_case('adapter',[R/'src/vendor_adapter.c',R/'src/encoded_transform.c'],R/'tests/vendor_adapter_test.c')
    def test_transport_async_lifecycle(self):
        self.run_case('transport',[T/'src/hal_transport.c'],T/'tests/transport_test.c')
    def test_raw_only_prepared_read_barrier(self):
        self.run_case('transport-prepared',[T/'src/hal_transport.c'],T/'tests/transport_test.c',
                      ['-DSHIZUKA_RELAY_RAW_ISLAND=1'])
    def test_positive_ack_before_waiter_wakes(self):
        self.run_case('ack',[R/'src/control_ack.c'],R/'tests/control_ack_test.c')
    def test_partial_graph_command_scope(self):
        self.run_case('scope',[R/'src/control_scope.c'],R/'tests/control_scope_test.c')
    def test_actual_augmented_init_profile(self):
        self.run_case('init-profile',[R/'src/init_profile.c'],R/'tests/init_profile_test.c')
    def test_receive_trace_is_bounded_and_nonblocking(self):
        self.run_case('trace',[R/'src/receive_trace.c'],R/'tests/receive_trace_test.c',['-pthread'])
    def test_data_receipt_trace_is_bounded_and_epoch_coherent(self):
        self.run_case('data-trace',[R/'src/data_receipt_trace.c'],R/'tests/data_receipt_trace_test.c',['-pthread'])
    def test_graph_and_calibration_owner_bridge(self):
        self.run_case('bridge',[R/'graph_bridge.c',*[G/n for n in ('relay_graph.c','relay_transaction.c','relay_calibration.c')]],R/'tests/graph_bridge_test.c')
    def test_default_off_raw_island_graph_experiment(self):
        self.run_case('raw-island',[G/n for n in ('relay_graph.c','relay_transaction.c','relay_calibration.c')],
                      G/'raw_island_graph_test.c',['-DSHIZUKA_RELAY_RAW_ISLAND=1'])
    def test_default_calibration_uses_actual_open_keys(self):
        self.run_case('default-cal',[R/'default_cal_context.c'],R/'tests/default_cal_context_test.c')

if __name__=='__main__':unittest.main()
