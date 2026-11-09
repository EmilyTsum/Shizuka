"""Offline ownership and exact-byte tests; no mount or phone access."""

import hashlib, os, pathlib, subprocess, tempfile, unittest

P = pathlib.Path(__file__).resolve().parents[1]


class Nvm(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = pathlib.Path(self.tmp.name)
        self.functions = (
            (P / "module/qhs-nvm.sh").read_text().split("# Sourceable function definitions")[0]
        )
        self.bb = self.d / "bb"
        self.bb.write_text("""#!/usr/bin/env python3
import sys,os,hashlib,subprocess
a=sys.argv[1:]
if a[0]=='stat':
 s=os.stat(a[-1]); print(str(s.st_dev)+':'+str(s.st_ino) if a[2]=='%d:%i' else '0:'+oct(s.st_mode&0o777)[2:])
elif a[0]=='sha256sum': print(hashlib.sha256(open(a[1],'rb').read()).hexdigest()+'  '+a[1])
else: sys.exit(subprocess.call(a))
""")
        self.bb.chmod(0o755)
        self.c = self.d / "candidate"
        self.c.write_bytes(b"known candidate")
        os.link(self.c, self.d / "target")
        for n, v in [("intent", ""), ("boot", "boot-A"), ("mount.id", "123")]:
            (self.d / n).write_text(v + "\n")

    def tearDown(self):
        self.tmp.cleanup()

    def check_owned(self, expected=True, boot="boot-A", extra=""):
        script = (
            self.functions
            + f"""
D='{self.d}'; T='{self.d}/target'; BB='{self.bb}'; TEST='{hashlib.sha256(b"known candidate").hexdigest()}'
mount_id() {{ echo 123; }}
cat() {{ if [ "$1" = /proc/sys/kernel/random/boot_id ];then echo '{boot}';else command cat "$@";fi; }}
{extra}
owned
"""
        )
        r = subprocess.run(["sh", "-c", script], capture_output=True)
        self.assertEqual(r.returncode == 0, expected, r.stderr.decode())

    def test_owned_same_inode_hash_id_boot(self):
        self.check_owned()

    def test_foreign_same_bytes_new_inode_rejected(self):
        (self.d / "target").unlink()
        (self.d / "target").write_bytes(b"known candidate")
        self.check_owned(False)

    def test_foreign_mountid_rejected(self):
        (self.d / "mount.id").write_text("124\n")
        self.check_owned(False)

    def test_stale_boot_rejected(self):
        self.check_owned(False, boot="boot-B")

    def test_symlink_candidate_rejected(self):
        self.c.unlink()
        self.c.symlink_to(self.d / "target")
        self.check_owned(False)

    def test_crash_before_mountid_publication_supported(self):
        (self.d / "mount.id").unlink()
        self.check_owned()

    def test_candidate_modified_rejected(self):
        self.c.write_bytes(b"wrong")
        self.check_owned(False)

    def test_no_intent_rejected(self):
        (self.d / "intent").unlink()
        self.check_owned(False)

    def test_default_false_does_not_invoke_helper(self):
        # Execute the actual opt-in activation block, with no config file present.
        s = (P / "module/control.sh").read_text()
        block = s[
            s.index("  qhs_enabled=false") : s.index(
                "  quiesce_radio || fail", s.index("  qhs_enabled=false")
            )
        ]
        r = subprocess.run(
            ["sh", "-c", block + '\n[ "$qhs_enabled" = false ]'], capture_output=True
        )
        self.assertEqual(r.returncode, 0, r.stderr.decode())

    def test_failed_owned_cleanup_retains_recovery_arm(self):
        state = self.d / "state"
        state.mkdir()
        q = state / "qhs-nvm"
        q.mkdir()
        (q / "intent").touch()
        (state / "armed").write_text("token")
        (state / "health-token").write_text("health")
        (self.d / "config.sh").write_text("exit 0\n")
        (self.d / "qhs-nvm.sh").write_text("exit 1\n")
        bb = self.d / "bb-control"
        bb.write_text(
            '#!/bin/sh\ncase "$1" in flock) exit 0;;sh) shift;exec sh "$@";;*) exit 99;;esac\n'
        )
        bb.chmod(0o755)
        source = (P / "module/control.sh").read_text()
        functions = (
            f'. "{P / "module/lib/runtime.sh"}"\n'
            + source[source.index("qhs_features()") : source.index('case "${1:-status}"')]
        )
        script = (
            f'MODDIR="{self.d}"; STATE="{state}"; BB="{bb}"\nfail() {{ exit 1; }}\n'
            + functions
            + "\nrollback\n"
        )
        r = subprocess.run(["sh", "-c", script], capture_output=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertTrue((state / "armed").exists())
        self.assertFalse((state / "health-token").exists())

    def test_activation_exit_trap_failed_remove_has_no_nested_recovery(self):
        state = self.d / "state"
        state.mkdir()
        q = state / "qhs-nvm"
        q.mkdir()
        (q / "intent").touch()
        (state / "armed").write_text("token")
        (state / "health-token").write_text("health")
        (self.d / "config.sh").write_text("exit 0\n")
        (self.d / "qhs-nvm.sh").write_text("exit 1\n")
        bb = self.d / "bb-trap"
        calls = self.d / "lock.calls"
        bb.write_text(
            '#!/bin/sh\ncase "$1" in flock) if [ -e "'
            + str(calls)
            + '" ];then echo recursive >&2;exit 97;fi; touch "'
            + str(calls)
            + '";exit 0;;sh) shift;exec sh "$@";;*) exit 99;;esac\n'
        )
        bb.chmod(0o755)
        source = (P / "module/control.sh").read_text()
        functions = (
            f'. "{P / "module/lib/runtime.sh"}"\n'
            + source[source.index("qhs_features()") : source.index('case "${1:-status}"')]
        )
        trapline = [
            line.strip()
            for line in source.splitlines()
            if line.strip().startswith("trap '") and line.strip().endswith("' EXIT")
        ][0]
        script = (
            f'MODDIR="{self.d}"; STATE="{state}"; BB="{bb}"\nfail() {{ exit 1; }}\n'
            + functions
            + "\n"
            + trapline
            + "\nexit 23\n"
        )
        # Run the actual EXIT body under both available host shells, with a hard
        # subprocess deadline. Nested rollback would hit the sentinel lock.
        for shell in ["sh", "bash"]:
            calls.unlink(missing_ok=True)
            r = subprocess.run([shell, "-c", script], capture_output=True, timeout=3)
            self.assertNotEqual(r.returncode, 0)
            self.assertNotIn(b"recursive", r.stderr)
            self.assertTrue(calls.exists())
            self.assertTrue((state / "armed").exists())
        self.assertIn(
            "trap - EXIT HUP INT TERM", functions.split("rollback() (", 1)[1].split("exec 7>", 1)[0]
        )

    def test_stale_boot_remove_preserves_current_scan_setting(self):
        (self.d / "scan").write_text("1")
        (self.d / "restoring").touch()
        src = (P / "module/qhs-nvm.sh").read_text()
        body = src.split(" remove)\n", 1)[1].split(" finish-remove)", 1)[0]
        script = (
            self.functions
            + f"""
D='{self.d}'; BB='{self.bb}'; T='{self.d}/target'; ORIG=stock
mount_id() {{ :; }}
sha() {{ echo stock; }}
cat() {{ if [ "$1" = /proc/sys/kernel/random/boot_id ];then echo boot-B;else command cat "$@";fi; }}
settings() {{ echo CALLED >&2; exit 99; }}
case remove in remove)
"""
            + body
            + "esac\n"
        )
        r = subprocess.run(["sh", "-c", script], capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertNotIn(b"CALLED", r.stderr)
        for n in ["scan", "intent", "restoring", "mount.id"]:
            self.assertFalse((self.d / n).exists())

    def shell_entry_case(self, initial, args):
        # Execute each actual entry guard through a fake fixed BusyBox that
        # emulates /proc executable readlink and records exec transitions.
        for name in ['control.sh','qhs-nvm.sh']:
            with tempfile.TemporaryDirectory() as td:
                d=pathlib.Path(td);mode=d/'mode';mode.write_text(initial);calls=d/'exec.calls'
                bb=d/'busybox';bb.write_text("#!/usr/bin/env python3\nimport sys,os,pathlib\np=pathlib.Path(__file__).parent;m=p/'mode';a=sys.argv[1:]\nif a[:2]==['readlink','-f']:\n if a[2]==str(p/'busybox'): print(str(p/'busybox'))\n elif m.read_text()=='unknown': print('')\n else: print(str(p/'busybox') if m.read_text()=='ash' else '/system/bin/mksh')\nelif a[0]=='sh':\n with (p/'exec.calls').open('a') as f:f.write('exec\\n')\n m.write_text('ash');os.execv('/bin/sh',['sh']+a[1:])\nelse:sys.exit(90)\n");bb.chmod(0o755)
                src=(P/'module'/name).read_text();start=src.index('# Android mksh');guard=src[start:src.index('\nfi\n',start)+4]
                entry=d/'entry.sh';entry.write_text(f'BB="{bb}"\n'+guard+"\nprintf '%s\\0' \"$@\"\n")
                env=dict(os.environ, QHS_ALREADY_REEXEC='1', shell_exe=str(bb), bb_exe=str(bb))
                r=subprocess.run(['sh',str(entry)]+args,capture_output=True,env=env,timeout=3)
                yield name,r,calls.read_text() if calls.exists() else ''
    def test_mksh_entry_reexec_preserves_hostile_argv(self):
        args=['rollback','space arg','$(touch sentinel)','--flag','']
        for name,r,calls in self.shell_entry_case('mksh',args):
            self.assertEqual(r.returncode,0,(name,r.stderr))
            self.assertEqual(r.stdout,b''.join(a.encode()+b'\0' for a in args))
            self.assertEqual(calls,'exec\n')
    def test_ash_entry_no_reexec_loop(self):
        for name,r,calls in self.shell_entry_case('ash',['pending']):
            self.assertEqual(r.returncode,0,(name,r.stderr));self.assertEqual(calls,'')
            self.assertEqual(r.stdout,b'pending\0')
    def test_unknown_shell_identity_fails_before_reexec(self):
        for name,r,calls in self.shell_entry_case('unknown',['apply']):
            self.assertNotEqual(r.returncode,0,name);self.assertEqual(calls,'')
            self.assertIn(b'Cannot identify shell executable',r.stderr)
    def test_syntax(self):
        for n in ["control.sh", "qhs-nvm.sh"]:
            subprocess.run(["sh", "-n", str(P / "module" / n)], check=True)


if __name__ == "__main__":
    unittest.main()
