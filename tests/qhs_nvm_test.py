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
        functions = source[source.index("qhs_pending()") : source.index('case "${1:-status}"')]
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
        functions = source[source.index("qhs_pending()") : source.index('case "${1:-status}"')]
        trapline = [
            line.strip()
            for line in source.splitlines()
            if line.strip().startswith("trap 'code=$?;")
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

    def test_syntax(self):
        for n in ["control.sh", "qhs-nvm.sh"]:
            subprocess.run(["sh", "-n", str(P / "module" / n)], check=True)


if __name__ == "__main__":
    unittest.main()
