# SPDX-License-Identifier: GPL-2.0-only
"""Run the real Samsung recovery hooks against fixed, private Android boundaries."""
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("SHIZUKA_TEST_SHELL", "sh"))
HAL = "/vendor/lib64/libsehbluetooth_audio_session_aidl.so"
GSL = "/vendor/lib64/libar-gsl.so"
LOADER = "/vendor_dlkm/lib/modules/adsp_loader_dlkm.ko"
DSP = "/vendor/dsp/adsp/aptx_adaptive_enc_module.so.1"
SERVICES = (
    "android.hardware.audio.core.IModule/default",
    "android.hardware.audio.effect.IFactory/default",
    "vendor.samsung.hardware.bluetooth.audio.ISehBluetoothAudioProviderFactory/default",
)


class ProfileRecovery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shizuka-normal-owner-")
        self.addCleanup(self.temp.cleanup)
        self.d = Path(self.temp.name)
        for name, text in {
            "adsp-name": "3000000.remoteproc-adsp\n", "adsp-state": "running\n",
            "ssr": "not requested", "crash": "not touched", "hal-pid": "7777\n",
            "hal-service": "running\n", "hal-start": "1234\n", "alive": "yes",
            "maps": f"1000-2000 r-xp 00000000 00:01 1 {HAL}\n2000-3000 r-xp 00000000 00:01 2 {GSL}\n",
        }.items():
            (self.d / name).write_text(text)
        source = (ROOT / "module/profiles/samsung-sm-s931z-dzif.sh").read_text()
        for original, fixture in {
            "/sys/class/remoteproc/remoteproc1/name": "adsp-name",
            "/sys/class/remoteproc/remoteproc1/state": "adsp-state",
            "/sys/kernel/boot_adsp/ssr": "ssr",
        }.items():
            source = source.replace(original, str(self.d / fixture))
        source = source.replace('"/proc/$1/maps"', '"$MODDIR/maps"')
        (self.d / "profile.sh").write_text(source)
        self.bb = self.d / "bb"
        self.bb.write_text(f"#!{sys.executable}\n" + r'''
import json,os,pathlib,sys
d=pathlib.Path(os.environ['TEST_ROOT']);a=sys.argv[1:]
with (d/'events').open('a') as out:out.write(json.dumps(a)+'\n')
if a[0]=='timeout':
 assert a[1]=='1'
 if a[2:4]==['dumpsys','--pid']:
  print('8888' if a[4]==os.environ.get('TEST_WRONG_BINDER') else '7777')
 elif a[2:4]==['service','call']:
  assert a[5]=='1599098439'
  if a[4]==os.environ.get('TEST_BLOCK_BINDER'):raise SystemExit(124)
  print('Result: Parcel(NULL)')
  if os.environ.get('TEST_REUSE_PID')=='1':(d/'hal-start').write_text('9999\n')
 else:raise SystemExit(91)
elif a[0]=='sha256sum':
 hashes={
 '/vendor_dlkm/lib/modules/adsp_loader_dlkm.ko':'3d9708cbb7b5e5b64bbb0f4acea91a6ba4db9630d9ac8503f7dda2a546d8ec1c',
 '/vendor/dsp/adsp/aptx_adaptive_enc_module.so.1':'05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3',
 '/vendor/lib64/libar-gsl.so':'4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc'}
 print(('f'*64 if a[1]==os.environ.get('TEST_BAD_HASH') else hashes[a[1]])+'  '+a[1])
else:os.execvp(a[0],a)
''')
        self.bb.chmod(0o755)
        self.boundaries = r'''
getprop() {
 case "$1" in
  ro.build.version.incremental) printf '%s\n' "${TEST_FIRMWARE:-S931ZSCU1DZIF}";;
  init.svc_debug_pid.vendor.audio-hal-aidl) cat "$MODDIR/hal-pid";;
  init.svc.vendor.audio-hal-aidl) cat "$MODDIR/hal-service";;
  *) return 90;;
 esac
}
setprop() {
 [ "$1:$2" = ctl.stop:vendor.audio-hal-aidl ] || return 91
 printf '%s\n' "$*" >> "$MODDIR/mutations"
 if [ "${TEST_STOP_STUCK:-0}" != 1 ];then
  : > "$MODDIR/hal-pid";echo stopped > "$MODDIR/hal-service";rm -f "$MODDIR/alive"
 fi
}
process_start() {
 [ "$1" = 7777 ] && [ -f "$MODDIR/alive" ] || return 1
 cat "$MODDIR/hal-start"
}
process_matches() { [ "$(process_start "$1")" = "$2" ]; }
nsrun() { shift;"$@"; }
sleep() { :; }
'''

    def run_hook(self, hook, *args, ok=True, **options):
        script = '. "$MODDIR/profile.sh"\n' + self.boundaries + hook + " " + " ".join(map(shlex.quote, args))
        result = subprocess.run(
            SHELL + ["-eu", "-c", script],
            env=dict(os.environ, MODDIR=str(self.d), BB=str(self.bb), TEST_ROOT=str(self.d), **options),
            text=True, capture_output=True, timeout=6,
        )
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def events(self):
        path = self.d / "events"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def test_preflight_is_exact_firmware_adsp_loader_stock_dsp_and_gsl(self):
        self.run_hook("profile_latency_recovery_preflight")
        for path in (LOADER, DSP, GSL):
            self.run_hook("profile_latency_recovery_preflight", ok=False, TEST_BAD_HASH=path)
        self.run_hook("profile_latency_recovery_preflight", ok=False, TEST_FIRMWARE="unreviewed")
        (self.d / "adsp-name").write_text("spss\n")
        self.run_hook("profile_latency_recovery_preflight", ok=False)
        self.assertFalse((self.d / "mutations").exists())
        self.assertEqual((self.d / "ssr").read_text(), "not requested")

    def test_verified_ssr_writes_only_one_to_the_normal_entry(self):
        self.run_hook("profile_latency_restart_dsp")
        self.assertEqual((self.d / "ssr").read_bytes(), b"1")
        self.assertEqual((self.d / "crash").read_text(), "not touched")
        self.assertFalse((self.d / "mutations").exists())

    def test_unreviewed_loader_or_symlink_entry_cannot_request_restart(self):
        self.run_hook("profile_latency_restart_dsp", ok=False, TEST_BAD_HASH=LOADER)
        self.assertEqual((self.d / "ssr").read_text(), "not requested")
        (self.d / "ssr").unlink()
        (self.d / "ssr").symlink_to(self.d / "crash")
        self.run_hook("profile_latency_restart_dsp", ok=False)
        self.assertEqual((self.d / "crash").read_text(), "not touched")

    def test_only_the_exact_normal_hal_owner_is_stopped(self):
        self.assertEqual(self.run_hook("profile_latency_hal_identity").stdout, "7777 1234\n")
        self.run_hook("profile_latency_stop_hal", "7777", "9999", ok=False)
        self.assertFalse((self.d / "mutations").exists())
        self.run_hook("profile_latency_stop_hal", "7777", "1234")
        self.assertEqual((self.d / "mutations").read_text(), "ctl.stop vendor.audio-hal-aidl\n")
        self.assertEqual(self.run_hook("profile_latency_hal_identity").stdout, "0 0\n")

    def test_stop_timeout_is_bounded_and_never_escalates_to_unrelated_service(self):
        self.run_hook("profile_latency_stop_hal", "7777", "1234", ok=False, TEST_STOP_STUCK="1")
        self.assertTrue((self.d / "alive").exists())
        self.assertEqual((self.d / "mutations").read_text(), "ctl.stop vendor.audio-hal-aidl\n")

    def test_full_ready_requires_all_three_binders_same_fresh_process(self):
        self.run_hook("profile_hal_ready", "7777", "6666")
        pings = [e for e in self.events() if e[:4] == ["timeout", "1", "service", "call"]]
        self.assertEqual([e[4] for e in pings], list(SERVICES))
        for service in SERVICES:
            self.run_hook("profile_hal_ready", "7777", "6666", ok=False, TEST_WRONG_BINDER=service)
            self.run_hook("profile_hal_ready", "7777", "6666", ok=False, TEST_BLOCK_BINDER=service)
        self.run_hook("profile_hal_ready", "7777", "7777", ok=False)
        self.run_hook("profile_hal_ready", "7777", "6666", ok=False, TEST_REUSE_PID="1")

    def test_full_ready_rejects_unrelated_or_deleted_hal_mapping(self):
        for mapping in (HAL + ".unrelated", HAL + " (deleted)", "/unrelated/" + Path(HAL).name):
            (self.d / "maps").write_text("1000-2000 r-xp 00000000 00:01 1 " + mapping + "\n")
            self.run_hook("profile_hal_ready", "7777", "6666", ok=False)

    def test_normal_ready_additionally_requires_running_adsp_and_stock_gsl(self):
        self.run_hook("profile_latency_normal_ready", "7777", "6666")
        self.run_hook("profile_latency_normal_ready", "7777", "6666", ok=False, TEST_BAD_HASH=GSL)
        (self.d / "adsp-state").write_text("offline\n")
        self.run_hook("profile_latency_normal_ready", "7777", "6666", ok=False)

    def test_normal_ready_rejects_deleted_or_unrelated_gsl_mapping(self):
        for mapping in (GSL + " (deleted)", GSL + ".unrelated"):
            (self.d / "maps").write_text(
                f"1000-2000 r-xp 00000000 00:01 1 {HAL}\n2000-3000 r-xp 00000000 00:01 2 {mapping}\n"
            )
            self.run_hook("profile_latency_normal_ready", "7777", "6666", ok=False)


if __name__ == "__main__":
    unittest.main()
