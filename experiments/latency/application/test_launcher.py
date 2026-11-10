#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real new launcher with synthetic app_process and real BSD flock."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
ID = "ed22fba8-41da-4c3b-baf7-15350e1acc7e"


class Launcher(unittest.TestCase):
    def setUp(self):
        previous_umask = os.umask(0o022)
        self.addCleanup(os.umask, previous_umask)
        self.temporary = tempfile.TemporaryDirectory(prefix="shizuka-apply-launcher-")
        self.addCleanup(self.temporary.cleanup)
        self.d = Path(self.temporary.name).resolve()
        (self.d / "state").mkdir(mode=0o700)
        (self.d / "lib").mkdir(mode=0o700)
        runtime = (ROOT / "module/lib/runtime.sh").read_text()
        runtime += '''
ensure_busybox_shell() { :; }
process_start() {
 fixture_state=$(ps -o stat= -p "$1") || return 1
 case "$fixture_state" in *Z*) return 1;;esac
 printf '%s\\n' 1234
}
'''
        (self.d / "lib/runtime.sh").write_text(runtime)
        source = (ROOT / "module/latency-apply.sh").read_text()
        source = source.replace("BB=/data/adb/ksu/bin/busybox", 'BB="$MODDIR/bb"')
        source = source.replace("APP_PROCESS=/system/bin/app_process", 'APP_PROCESS="$MODDIR/app_process"')
        source = source.replace('[ "$(id -u)" = 0 ]', '[ 0 = 0 ]')
        self.entry = self.d / "latency-apply.sh"
        self.entry.write_text(source)
        (self.d / "latency-apply-guardian.sh").write_text((ROOT / "module/latency-apply-guardian.sh").read_text())
        (self.d / "latency-apply-executor.sh").write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "${0%/*}/executor-calls"\nexit 0\n')
        dex = b"synthetic DEX placeholder, never passed to Android"
        (self.d / "latency-application.dex").write_bytes(dex)
        (self.d / "latency-application.sha256").write_text(hashlib.sha256(dex).hexdigest() + "\n")
        bb = self.d / "bb"
        bb.write_text(f"#!{sys.executable}\n" + r'''
import fcntl,hashlib,os,pathlib,stat,sys,time
a=sys.argv[1:]
if a[0]=='stat':
 s=pathlib.Path(a[3]).stat();fmt=a[2]
 if fmt=='%u': print('0')
 elif fmt=='%u:%a':print('0:'+format(stat.S_IMODE(s.st_mode),'o'))
 elif fmt=='%u:%a:%h':print('0:'+format(stat.S_IMODE(s.st_mode),'o')+':'+str(s.st_nlink))
 elif fmt=='%s':print(s.st_size)
 else:raise SystemExit('unexpected stat '+fmt)
elif a[0]=='readlink':print(pathlib.Path(a[-1]).resolve())
elif a[0]=='sha256sum':print(hashlib.sha256(pathlib.Path(a[1]).read_bytes()).hexdigest()+'  '+a[1])
elif a[0]=='flock':
 deadline=time.monotonic()+float(a[2]);fd=int(a[3])
 while True:
  try:fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);break
  except BlockingIOError:
   if time.monotonic()>=deadline:raise SystemExit(1)
   time.sleep(.01)
elif a[0]=='setsid':os.setsid();os.execv(a[1],a[1:])
elif a[0]=='sh':os.execv('/bin/sh',['sh',*a[1:]])
elif a[0]=='timeout':os.execv(a[4],a[4:])
elif a[0]=='env':
 key,value=a[1].split('=',1);os.environ[key]=value;os.execv(a[2],a[2:])
elif a[0]=='tr':sys.stdout.write(sys.stdin.read().replace('-',''))
elif a[0]=='mktemp':
 import tempfile
 fd,name=tempfile.mkstemp(prefix='.response.',dir=str(pathlib.Path(a[1]).parent));os.close(fd);print(name)
else:raise SystemExit('unexpected busybox '+str(a))
''')
        bb.chmod(0o700)
        app = self.d / "app_process"
        app.write_text(f"#!{sys.executable}\n" + r'''
import json,os,pathlib,sys,time
d=pathlib.Path(os.environ['FIXTURE_ROOT']);a=sys.argv[1:];command=a[4]
try:os.fstat(9);inherited=True
except OSError:inherited=False
with (d/'calls').open('a') as out:out.write(json.dumps({'command':command,'arguments':a,'lock_inherited':inherited})+'\n')
if command=='prepare':
 accepted=os.environ.get('FIXTURE_QUEUED')=='1'
 print(json.dumps({'schema':1,'accepted':accepted,'applied':False,'phase':'queued' if accepted else 'rejected'}))
 raise SystemExit(10 if accepted else 0)
if command=='worker':
 (d/'working').write_text('1')
 if os.environ.get('FIXTURE_CHANNEL')=='1':
  channel=d/'state/latency-application'/('channel.'+a[5]);request=channel/'request'
  request.write_text('SHZAPPLY1 1 '+os.environ.get('FIXTURE_ACTION','1')+' '+a[5]+' 150 0 '+str(int(time.monotonic()*1000)+1000)+' '+'a'*64+' '+'b'*64+' 0\n');request.chmod(0o600)
  until=time.monotonic()+.8
  while not (channel/'response').exists() and time.monotonic()<until:time.sleep(.01)
  if (channel/'response').exists():(d/'reply').write_text((channel/'response').read_text())
 time.sleep(float(os.environ.get('FIXTURE_WORK_SECONDS','0')))
 (d/'finished').write_text('1')
 print('{"phase":"unconfirmed","applied":false}')
else:print('{"phase":"saved_pending","applied":false}')
''')
        app.chmod(0o700)
        self.env = {**os.environ, "FIXTURE_ROOT": str(self.d)}

    def run_entry(self, *args, check=True):
        return subprocess.run(["sh", str(self.entry), *args], env=self.env, capture_output=True,
                              text=True, timeout=5, check=check)

    def calls(self):
        path = self.d / "calls"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def wait_file(self, name):
        until = time.monotonic() + 4
        while not (self.d / name).exists() and time.monotonic() < until:
            time.sleep(.01)
        self.assertTrue((self.d / name).exists(), name)

    def test_unqualified_never_starts_worker(self):
        result = self.run_entry("begin", ID, "150", "100")
        self.assertEqual(json.loads(result.stdout)["phase"], "rejected")
        self.assertEqual([c["command"] for c in self.calls()], ["prepare"])
        self.assertFalse(self.calls()[0]["lock_inherited"])

    def test_async_guardian_reacquires_same_lock_and_child_does_not_inherit(self):
        self.env.update(FIXTURE_QUEUED="1", FIXTURE_WORK_SECONDS=".6")
        result = self.run_entry("begin", ID, "150", "0")
        self.assertEqual(json.loads(result.stdout)["phase"], "queued")
        self.wait_file("working")
        busy = self.run_entry("begin", ID, "150", "0")
        self.assertIn("busy", json.loads(busy.stdout)["reason"])
        self.assertEqual([c["command"] for c in self.calls()], ["prepare", "worker"])
        self.assertTrue(all(not c["lock_inherited"] for c in self.calls()))
        self.wait_file("finished")

    def test_existing_lifecycle_lock_blocks_launch_but_not_readonly_status(self):
        with (self.d / "state/lifecycle.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.assertFalse(json.loads(self.run_entry("begin", ID, "150", "0").stdout)["accepted"])
            self.assertEqual(self.calls(), [])
            self.assertEqual(json.loads(self.run_entry("status").stdout)["phase"], "saved_pending")
        self.assertEqual([c["command"] for c in self.calls()], ["status"])

    def test_changed_artifact_rejected_before_java(self):
        (self.d / "latency-application.dex").write_bytes(b"changed")
        self.assertNotEqual(self.run_entry("begin", ID, "150", "0", check=False).returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_group_writable_packaged_file_is_rejected(self):
        self.entry.chmod(0o664)
        self.assertNotEqual(self.run_entry("begin", ID, "150", "0", check=False).returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_typed_private_request_reaches_fixed_executor_and_bound_reply(self):
        self.env.update(FIXTURE_QUEUED="1", FIXTURE_CHANNEL="1")
        self.run_entry("begin", ID, "150", "0")
        self.wait_file("finished")
        self.assertEqual((self.d / "reply").read_text(), f"SHZAPPLY1 1 1 {ID} 0\n")
        fields = (self.d / "executor-calls").read_text().strip().split(" ")
        self.assertEqual(fields[:4], ["1", ID, "150", "0"])
        self.assertEqual(len(fields), 11)

    def test_noncanonical_action_never_executes(self):
        self.env.update(FIXTURE_QUEUED="1", FIXTURE_CHANNEL="1", FIXTURE_ACTION="1;touch")
        self.run_entry("begin", ID, "150", "0")
        self.wait_file("finished")
        self.assertFalse((self.d / "executor-calls").exists())
        self.assertFalse((self.d / "reply").exists())

    def test_disable_and_digest_symlink_rejected(self):
        (self.d / "disable").touch()
        self.assertNotEqual(self.run_entry("status", check=False).returncode, 0)
        (self.d / "disable").unlink()
        digest = self.d / "latency-application.sha256"
        digest.rename(self.d / "digest")
        digest.symlink_to(self.d / "digest")
        self.assertNotEqual(self.run_entry("status", check=False).returncode, 0)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
